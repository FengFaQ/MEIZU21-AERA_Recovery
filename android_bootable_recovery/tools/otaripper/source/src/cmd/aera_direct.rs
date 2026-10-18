// Copyright 2026 AERA Recovery Project contributors.
// SPDX-License-Identifier: Apache-2.0
// Direct write phase only. AERA owns target selection, unmount/resizing, exclusive
// claim and mandatory flushed readback against the reviewed manifest afterward.
use super::Cmd;
use crate::payload::{Payload, PayloadData};
use crate::proto::chromeos_update_engine::{DeltaArchiveManifest, PartitionUpdate};
use anyhow::{bail, ensure, Context, Result};
use rayon::prelude::*;
use ring::digest::{digest, SHA256};
use std::fs::File;
use std::io::Read;
use std::os::fd::{AsRawFd, BorrowedFd};
use std::os::unix::fs::{FileExt, FileTypeExt, MetadataExt};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, AtomicU8, Ordering};
use std::collections::HashSet;
use std::time::Duration;

const LIMIT: u64 = 64 * 1024 * 1024;
const WRITE_CHUNK: usize = 1024 * 1024;

fn log_cpu_affinity(label: &str) {
    match std::fs::read_to_string("/proc/thread-self/status") {
        Ok(status) => {
            let allowed = status.lines().find_map(|line| line.strip_prefix("Cpus_allowed_list:"))
                .unwrap_or("unavailable").trim();
            eprintln!("AERA_AFFINITY {label}: CPUs {allowed}");
        }
        Err(e) => eprintln!("AERA_AFFINITY {label}: unavailable ({e})"),
    }
}

fn validate(p: &PartitionUpdate, block: u64, data: &[u8]) -> Result<u64> {
    ensure!(block > 0 && block <= 1024 * 1024 && block.is_power_of_two(), "Invalid block size");
    ensure!(p.old_partition_info.is_none(), "Incremental image is not supported");
    let info = p.new_partition_info.as_ref().context("Missing image information")?;
    let size = info.size.context("Missing image size")?;
    ensure!(size > 0 && size <= i64::MAX as u64 && size % block == 0, "Invalid image size");
    ensure!(info.hash.as_ref().is_some_and(|h| h.len() == 32), "Missing image SHA-256");
    let mut ranges = Vec::new();
    for op in &p.operations {
        ensure!(matches!(op.r#type, 0 | 1 | 6 | 7 | 8 | 14) && op.src_extents.is_empty() &&
            op.src_sha256_hash.is_none(), "Unsupported operation");
        let mut decoded = 0u64;
        for extent in &op.dst_extents {
            let start = extent.start_block.context("Missing extent offset")?;
            let count = extent.num_blocks.context("Missing extent length")?;
            ensure!(count > 0 && start < size / block && count <= size / block - start, "Extent outside image");
            decoded = decoded.checked_add(count * block).context("Extent overflow")?;
            ranges.push((start * block, (start + count) * block));
            ensure!(ranges.len() <= 1_000_000, "Too many extents");
        }
        ensure!(decoded > 0, "Empty operation");
        let length = op.data_length.unwrap_or(0);
        if matches!(op.r#type, 6 | 7) {
            ensure!(length == 0, "Unexpected zero-operation data");
        } else {
            let offset = op.data_offset.context("Missing data offset")?;
            ensure!(decoded <= LIMIT && length > 0 && length <= LIMIT, "Operation exceeds memory limit");
            ensure!(offset <= data.len() as u64 && length <= data.len() as u64 - offset, "Truncated operation");
            ensure!(op.data_sha256_hash.as_ref().is_some_and(|h| h.len() == 32), "Missing operation hash");
        }
    }
    ranges.sort_unstable();
    let mut end = 0;
    for (start, next) in ranges {
        ensure!(start == end, "Overlapping or incomplete extents");
        end = next;
    }
    ensure!(end == size, "Incomplete image");
    Ok(size)
}

// Regular files are accepted only by this private worker to allow non-destructive
// unit tests. The command entry below rejects everything except block devices.
fn write_operation(file: &File, op: &crate::proto::chromeos_update_engine::InstallOperation,
                   block: u64, data: &[u8], cancelled: &AtomicBool, done: &AtomicU64) -> Result<()> {
        if cancelled.load(Ordering::Acquire) { bail!("Direct flash cancelled after write error"); }
        let result = (|| -> Result<()> {
            let decoded_size = op.dst_extents.iter().map(|e| e.num_blocks.unwrap() * block).sum::<u64>();
            let zero = matches!(op.r#type, 6 | 7);
            let mut reader: Box<dyn Read + '_> = if zero {
                Box::new(std::io::repeat(0))
            } else {
                let offset = op.data_offset.unwrap() as usize;
                let length = op.data_length.unwrap() as usize;
                let input = &data[offset..offset + length];
                ensure!(digest(&SHA256, input).as_ref() == op.data_sha256_hash.as_ref().unwrap(), "Operation SHA-256 mismatch");
                // Check the compressed operation before its first write. Decode
                // into bounded chunks rather than retaining the whole output.
                match op.r#type {
                    0 => {
                        ensure!(input.len() as u64 == decoded_size, "Raw operation size mismatch");
                        Box::new(input)
                    }
                    1 => Box::new(bzip2::read::BzDecoder::new(input)),
                    8 => {
                        let stream = liblzma::stream::Stream::new_stream_decoder(80 * 1024 * 1024, 0)?;
                        Box::new(liblzma::read::XzDecoder::new_stream(input, stream))
                    }
                    14 => {
                        let mut decoder = zstd::stream::read::Decoder::new(input)?;
                        decoder.window_log_max(27)?;
                        Box::new(decoder)
                    }
                    _ => unreachable!(),
                }
            };
            let mut buffer = vec![0u8; WRITE_CHUNK];
            for extent in &op.dst_extents {
                let mut offset = extent.start_block.unwrap() * block;
                let mut left = extent.num_blocks.unwrap() * block;
                while left > 0 {
                    ensure!(!cancelled.load(Ordering::Acquire), "Direct flash cancelled");
                    let count = left.min(WRITE_CHUNK as u64) as usize;
                    reader.read_exact(&mut buffer[..count]).context("Truncated or corrupt decoded operation")?;
                    ensure!(!cancelled.load(Ordering::Acquire), "Direct flash cancelled");
                    file.write_all_at(&buffer[..count], offset).context("Partition write failed")?;
                    offset += count as u64;
                    left -= count as u64;
                    done.fetch_add(count as u64, Ordering::Relaxed);
                }
            }
            // Drive the decoder to EOF, including checksum/footer validation.
            // An error here leaves an incomplete target: never mark it written.
            if !zero {
                let mut extra = [0u8; 1];
                ensure!(reader.read(&mut extra).context("Invalid decoder trailer")? == 0,
                    "Decoded operation exceeds destination extents");
            }
            Ok(())
        })();
        if result.is_err() { cancelled.store(true, Ordering::Release); }
        result
}

// One operation-level work-stealing pool over ALL selected targets. Only the
// worker count limits concurrent operations; no partition windows or barriers.
fn write_batch(files: &[&File], parts: &[&PartitionUpdate], block: u64, data: &[u8], workers: usize) -> Result<()> {
    ensure!(!parts.is_empty() && parts.len() <= 256 && files.len() == parts.len(), "Invalid target mapping");
    let sizes: Vec<u64> = parts.iter().map(|p| validate(p, block, data)).collect::<Result<_>>()?;
    let total_size = sizes.iter().try_fold(0u64, |sum, size| sum.checked_add(*size)).context("Batch size overflow")?;
    let mut identities = HashSet::new();
    let mut names = HashSet::new();
    for (file, p) in files.iter().zip(parts) {
        let m = file.metadata()?;
        let identity = if m.file_type().is_block_device() { (0, m.rdev()) } else { (m.dev(), m.ino()) };
        ensure!(identities.insert(identity) && names.insert(&p.partition_name), "Duplicate direct target");
        ensure!(!p.partition_name.is_empty() && p.partition_name.len() <= 128 &&
            p.partition_name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-'), "Invalid partition name");
    }
    let workers = workers.clamp(1, 16);
    log_cpu_affinity("otaripper launcher");
    let pool = rayon::ThreadPoolBuilder::new().num_threads(workers).build()?;
    let cancelled = AtomicBool::new(false);
    let stop = AtomicBool::new(false);
    let done: Vec<AtomicU64> = parts.iter().map(|_| AtomicU64::new(0)).collect();
    let phase: Vec<AtomicU8> = parts.iter().map(|_| AtomicU8::new(0)).collect();
    let remaining: Vec<AtomicUsize> = parts.iter().map(|p| AtomicUsize::new(p.operations.len())).collect();
    let report = || {
        let mut total_done = 0u64;
        for (i, p) in parts.iter().enumerate() {
            let state = phase[i].load(Ordering::Acquire);
            let count = done[i].load(Ordering::Relaxed);
            total_done += count;
            if state != 0 {
                let label = match state { 1 => "writing", 2 => "written", _ => "failed" };
                println!("AERA_PARTITION {} {} {} {}", p.partition_name, label, count, sizes[i]);
            }
        }
        println!("AERA_PROGRESS {} {}", total_done, total_size);
    };
    std::thread::scope(|scope| -> Result<()> {
        let reporter = std::thread::Builder::new().name("aera-progress".into()).spawn_scoped(scope, || {
            while !stop.load(Ordering::Acquire) {
                report();
                std::thread::sleep(Duration::from_millis(100));
            }
        })?;
        let mut order: Vec<usize> = (0..parts.len()).collect();
        order.sort_by_key(|&i| std::cmp::Reverse(sizes[i]));
        let result = (|| -> Result<()> {
                let max_ops = order.iter().map(|&i| parts[i].operations.len()).max().unwrap();
                let mut tasks = Vec::new();
                // Interleave operations from every target. Any worker can take
                // the next operation, including one from a different partition.
                for op in 0..max_ops {
                    for &i in &order { if op < parts[i].operations.len() { tasks.push((i, op)); } }
                }
                pool.install(|| tasks.par_iter().try_for_each(|&(i, op)| -> Result<()> {
                    ensure!(!cancelled.load(Ordering::Acquire), "Direct flash cancelled");
                    let _ = phase[i].compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire);
                    let result = write_operation(files[i], &parts[i].operations[op], block, data, &cancelled, &done[i])
                        .and_then(|_| {
                            if remaining[i].fetch_sub(1, Ordering::AcqRel) == 1 {
                                files[i].sync_all().context("Partition flush failed")?;
                                phase[i].store(2, Ordering::Release);
                            }
                            Ok(())
                        });
                    if result.is_err() {
                        cancelled.store(true, Ordering::Release);
                        phase[i].store(3, Ordering::Release);
                    }
                    result.with_context(|| format!("Direct target {}", parts[i].partition_name))
                }))?;
            Ok(())
        })();
        stop.store(true, Ordering::Release);
        reporter.join().map_err(|_| anyhow::anyhow!("Progress reporter failed"))?;
        report();
        result
    })
}

#[cfg(test)]
fn write_image(file: &File, p: &PartitionUpdate, block: u64, data: &[u8], workers: usize) -> Result<()> {
    write_batch(&[file], &[p], block, data, workers)
}

pub(super) fn run(cmd: &Cmd, payload: &Payload<'_>, manifest: &DeltaArchiveManifest, block: usize) -> Result<()> {
    ensure!(cmd.aera && cmd.strict && !cmd.no_verify && !cmd.partitions.is_empty() &&
        cmd.partitions.len() == cmd.aera_block_fd.len(), "Invalid direct request");
    let expected = hex::decode(cmd.aera_manifest.as_ref().context("Missing reviewed manifest")?)?;
    ensure!(expected.len() == 32 && digest(&SHA256, payload.manifest).as_ref() == expected, "Manifest changed since review");
    let data = match &payload.data {
        PayloadData::Local(bytes) => *bytes,
        #[cfg(feature = "remote")]
        _ => bail!("Direct mode requires a local package"),
    };
    let mut files = Vec::new();
    let mut parts = Vec::new();
    for (name, &fd) in cmd.partitions.iter().zip(&cmd.aera_block_fd) {
    let p = manifest.partitions.iter().find(|p| &p.partition_name == name).context("Missing selected partition")?;
    let size = validate(p, block as u64, data)?;
    ensure!(fd >= 3, "Invalid block descriptor");
    ensure!(unsafe { libc::fcntl(fd, libc::F_GETFD) } >= 0, "Closed block descriptor");
    // Clone the inherited descriptor; never reopen a user-supplied target path.
    let file = File::from(unsafe { BorrowedFd::borrow_raw(fd) }.try_clone_to_owned()?);
    ensure!(file.metadata()?.file_type().is_block_device(), "Direct target is not a block device");
    let mut capacity = 0u64;
    let mut readonly = 1i32;
    ensure!(unsafe { libc::ioctl(file.as_raw_fd(), 0x80081272u32 as _, &mut capacity) } == 0 &&
        unsafe { libc::ioctl(file.as_raw_fd(), 0x125e as _, &mut readonly) } == 0 &&
        readonly == 0 && capacity >= size, "Target is read-only or too small");
    files.push(file);
    parts.push(p);
    }
    write_batch(&files.iter().collect::<Vec<_>>(), &parts, block as u64, data, cmd.threads.unwrap_or(1))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::chromeos_update_engine::{Extent, InstallOperation, PartitionInfo};
    use std::io::Write;

    fn operation(kind: i32, start: u64, blocks: u64, input: &[u8]) -> InstallOperation {
        InstallOperation {
            r#type: kind,
            dst_extents: vec![Extent { start_block: Some(start), num_blocks: Some(blocks) }],
            data_offset: Some(0), data_length: Some(input.len() as u64),
            data_sha256_hash: Some(digest(&SHA256, input).as_ref().to_vec()),
            ..Default::default()
        }
    }
    fn partition(ops: Vec<InstallOperation>, size: u64) -> PartitionUpdate {
        PartitionUpdate { partition_name: "system".into(), operations: ops,
            new_partition_info: Some(PartitionInfo { size: Some(size), hash: Some(vec![0; 32]) }),
            ..Default::default() }
    }
    fn contents(file: &File, size: usize) -> Vec<u8> {
        let mut bytes = vec![0; size];
        file.read_exact_at(&mut bytes, 0).unwrap(); bytes
    }
    #[test]
    fn shared_pool_writes_all_targets_without_partition_windows() {
        let files: Vec<File> = (0..32).map(|_| tempfile::tempfile().unwrap()).collect();
        let mut data = Vec::new();
        let mut parts = Vec::new();
        for (i, file) in files.iter().enumerate() {
            file.write_all_at(&vec![0xee; 2048], 0).unwrap();
            let input = vec![(i + 1) as u8; 512];
            let offset = data.len() as u64;
            let mut op = operation(0, 0, 1, &input);
            op.data_offset = Some(offset);
            data.extend(input);
            let mut p = partition(vec![op, operation(6, 1, 2, &[])], 1536);
            p.partition_name = format!("test_{i}");
            parts.push(p);
        }
        write_batch(&files.iter().collect::<Vec<_>>(), &parts.iter().collect::<Vec<_>>(), 512, &data, 16).unwrap();
        for (i, file) in files.iter().enumerate() {
            let bytes = contents(file, 2048);
            assert_eq!(&bytes[..512], &vec![(i + 1) as u8; 512]);
            assert_eq!(&bytes[512..1536], &vec![0; 1024]);
            assert_eq!(&bytes[1536..], &vec![0xee; 512]);
        }
    }
    #[test]
    fn all_targets_validated_before_first_write_and_aliases_rejected() {
        let a = tempfile::tempfile().unwrap();
        let b = tempfile::tempfile().unwrap();
        for f in [&a, &b] { f.write_all_at(&vec![0x55; 512], 0).unwrap(); }
        let data = vec![7; 512];
        let valid = partition(vec![operation(0, 0, 1, &data)], 512);
        let mut other = partition(vec![operation(0, 1, 1, &data)], 512);
        other.partition_name = "vendor".into();
        assert!(write_batch(&[&a, &b], &[&valid, &other], 512, &data, 16).is_err());
        assert_eq!(contents(&a, 512), vec![0x55; 512]);
        assert_eq!(contents(&b, 512), vec![0x55; 512]);
        other.operations[0].dst_extents[0].start_block = Some(0);
        assert!(write_batch(&[&a, &a], &[&valid, &other], 512, &data, 16).is_err());
        assert!(write_batch(&[&a, &b], &[&valid, &valid], 512, &data, 16).is_err());
        assert_eq!(contents(&a, 512), vec![0x55; 512]);
    }
    #[test]
    fn corrupt_operation_stops_later_tasks_with_single_worker() {
        let a = tempfile::tempfile().unwrap();
        let b = tempfile::tempfile().unwrap();
        for f in [&a, &b] { f.write_all_at(&vec![0x55; 512], 0).unwrap(); }
        let data = vec![7; 512];
        let mut bad = partition(vec![operation(0, 0, 1, &data)], 512);
        bad.operations[0].data_sha256_hash = Some(vec![0; 32]);
        let mut next = partition(vec![operation(0, 0, 1, &data)], 512);
        next.partition_name = "vendor".into();
        assert!(write_batch(&[&a, &b], &[&bad, &next], 512, &data, 1).is_err());
        assert_eq!(contents(&a, 512), vec![0x55; 512]);
        assert_eq!(contents(&b, 512), vec![0x55; 512]);
    }
    #[test]
    fn zeros_replace_old_contents_and_larger_target_tail_survives() {
        let file = tempfile::tempfile().unwrap();
        file.write_all_at(&vec![0x55; 2048], 0).unwrap();
        let data = vec![0xab; 512];
        let p = partition(vec![operation(0, 0, 1, &data), operation(6, 1, 1, &[]), operation(7, 2, 1, &[])], 1536);
        write_image(&file, &p, 512, &data, 4).unwrap();
        let result = contents(&file, 2048);
        assert_eq!(&result[..512], &data);
        assert_eq!(&result[512..1536], &vec![0; 1024]);
        assert_eq!(&result[1536..], &vec![0x55; 512]);
    }
    #[test]
    fn codecs_write_multiple_extents_in_manifest_order() {
        let plain: Vec<u8> = (0..1024).map(|n| (n / 512 + 1) as u8).collect();
        for kind in [0, 1, 8, 14] {
            let input = match kind {
                1 => {
                    let mut e = bzip2::write::BzEncoder::new(Vec::new(), bzip2::Compression::best());
                    e.write_all(&plain).unwrap(); e.finish().unwrap()
                }
                8 => {
                    let mut e = liblzma::write::XzEncoder::new(Vec::new(), 6);
                    e.write_all(&plain).unwrap(); e.finish().unwrap()
                }
                14 => zstd::encode_all(&plain[..], 3).unwrap(),
                _ => plain.clone(),
            };
            let mut op = operation(kind, 1, 1, &input);
            op.dst_extents.push(Extent { start_block: Some(0), num_blocks: Some(1) });
            let p = partition(vec![op], 1024);
            let file = tempfile::tempfile().unwrap();
            file.set_len(1024).unwrap();
            write_image(&file, &p, 512, &input, 2).unwrap();
            assert_eq!(contents(&file, 1024), [vec![2; 512], vec![1; 512]].concat());
        }
    }
    #[test]
    fn invalid_coverage_and_source_bounds_fail_before_any_write() {
        let data = vec![7; 512];
        for bad in [operation(0, 0, 1, &data), operation(0, 2, 1, &data), operation(4, 1, 1, &data)] {
            let file = tempfile::tempfile().unwrap();
            file.write_all_at(&vec![0x55; 1024], 0).unwrap();
            let p = partition(vec![operation(0, 0, 1, &data), bad], 1024);
            assert!(write_image(&file, &p, 512, &data, 4).is_err());
            assert_eq!(contents(&file, 1024), vec![0x55; 1024]);
        }
        let p = partition(vec![operation(0, 0, 1, &data)], 512);
        assert!(validate(&p, 512, &data[..511]).is_err());
    }
    #[test]
    fn bad_operation_hash_or_decode_length_does_not_write_operation() {
        for mismatch in [true, false] {
            let file = tempfile::tempfile().unwrap();
            file.write_all_at(&vec![0x55; 512], 0).unwrap();
            let data = vec![7; if mismatch { 512 } else { 511 }];
            let mut op = operation(0, 0, 1, &data);
            if mismatch { op.data_sha256_hash = Some(vec![0; 32]); }
            let p = partition(vec![op], 512);
            assert!(write_image(&file, &p, 512, &data, 1).is_err());
            assert_eq!(contents(&file, 512), vec![0x55; 512]);
        }
    }
    #[test]
    fn streaming_codecs_cross_chunks_and_extents_without_touching_tail() {
        let size = WRITE_CHUNK * 3;
        let plain: Vec<u8> = (0..size).map(|i| ((i / 512) % 251) as u8).collect();
        for kind in [0, 1, 8] {
            let data = match kind {
                0 => plain.clone(),
                1 => {
                    let mut e = bzip2::write::BzEncoder::new(Vec::new(), bzip2::Compression::fast());
                    e.write_all(&plain).unwrap(); e.finish().unwrap()
                }
                _ => {
                    let mut e = liblzma::write::XzEncoder::new(Vec::new(), 1);
                    e.write_all(&plain).unwrap(); e.finish().unwrap()
                }
            };
            let mut op = operation(kind, 1, (size / 512 - 1) as u64, &data);
            op.dst_extents.push(Extent { start_block: Some(0), num_blocks: Some(1) });
            let file = tempfile::tempfile().unwrap();
            file.write_all_at(&vec![0xee; size + 512], 0).unwrap();
            write_image(&file, &partition(vec![op], size as u64), 512, &data, 1).unwrap();
            let expected = [&plain[size - 512..], &plain[..size - 512], &vec![0xee; 512]].concat();
            assert_eq!(contents(&file, size + 512), expected);
        }
    }
    #[test]
    fn streaming_rejects_short_and_overlong_decoded_output() {
        for size in [WRITE_CHUNK + 511, WRITE_CHUNK + 513] {
            let mut e = liblzma::write::XzEncoder::new(Vec::new(), 1);
            e.write_all(&vec![7; size]).unwrap();
            let data = e.finish().unwrap();
            let target_size = WRITE_CHUNK + 512;
            let file = tempfile::tempfile().unwrap();
            file.write_all_at(&vec![0xee; target_size + 512], 0).unwrap();
            let op = operation(8, 0, (target_size / 512) as u64, &data);
            assert!(write_image(&file, &partition(vec![op], target_size as u64), 512, &data, 1).is_err());
            assert_eq!(&contents(&file, target_size + 512)[target_size..], &vec![0xee; 512]);
        }
    }
    #[test]
    fn zstd_rejects_invalid_frames_and_output_lengths() {
        for fault in 0..5 {
            let plain = vec![7; if fault == 3 { 511 } else if fault == 4 { 513 } else { 512 }];
            let mut data = zstd::encode_all(&plain[..], 3).unwrap();
            if fault == 0 { data.pop(); }
            if fault == 1 { data[0] ^= 0xff; }
            if fault == 2 { data.push(0xff); }
            let file = tempfile::tempfile().unwrap();
            file.write_all_at(&vec![0xee; 1024], 0).unwrap();
            let p = partition(vec![operation(14, 0, 1, &data)], 512);
            assert!(write_image(&file, &p, 512, &data, 1).is_err());
            assert_eq!(&contents(&file, 1024)[512..], &vec![0xee; 512]);
        }
    }
    #[test]
    fn command_rejects_regular_file_even_with_valid_manifest() {
        use clap::Parser;
        use prost::Message;
        let file = tempfile::tempfile().unwrap();
        file.write_all_at(&vec![0x55; 512], 0).unwrap();
        let data = vec![7; 512];
        let p = partition(vec![operation(0, 0, 1, &data)], 512);
        let manifest = DeltaArchiveManifest { partitions: vec![p], block_size: Some(512), ..Default::default() };
        let bytes = manifest.encode_to_vec();
        let payload = Payload { file_format_version: 2, manifest_size: bytes.len() as u64,
            manifest: &bytes, metadata_signature: None, data: PayloadData::Local(&data) };
        let hash = hex::encode(digest(&SHA256, &bytes));
        let fd = file.as_raw_fd().to_string();
        let cmd = Cmd::try_parse_from(["otaripper", "--aera", "--strict", "--no-open", "--output-dir", "/tmp",
            "--partitions", "system", "--aera-block-fd", &fd, "--aera-manifest", &hash, "input.zip"]).unwrap();
        assert!(run(&cmd, &payload, &manifest, 512).unwrap_err().to_string().contains("not a block"));
        assert_eq!(contents(&file, 512), vec![0x55; 512]);
    }
    #[test]
    #[ignore = "Requires AERA_TEST_FULL_OTA; writes only a temporary regular file"]
    fn real_full_ota_direct_writer() {
        use prost::Message;
        let path = std::env::var("AERA_TEST_FULL_OTA").unwrap();
        let source = File::open(path).unwrap();
        let mapped = unsafe { memmap2::Mmap::map(&source) }.unwrap();
        let mut zip = zip::ZipArchive::new(&source).unwrap();
        let entry = zip.by_name("payload.bin").unwrap();
        assert_eq!(entry.compression(), zip::CompressionMethod::Stored);
        let start = entry.data_start().unwrap() as usize;
        let payload = Payload::parse(&mapped[start..start + entry.size() as usize]).unwrap();
        let manifest = DeltaArchiveManifest::decode(payload.manifest).unwrap();
        let p = manifest.partitions.iter().find(|p| p.partition_name == "system").unwrap();
        let PayloadData::Local(data) = payload.data else { panic!("Not local"); };
        let size = p.new_partition_info.as_ref().unwrap().size.unwrap();
        let output = tempfile::tempfile().unwrap();
        output.set_len(size + 512).unwrap();
        output.write_all_at(&vec![0x55; 512], size).unwrap();
        let start = std::time::Instant::now();
        write_image(&output, p, manifest.block_size.unwrap() as u64, data, 4).unwrap();
        let result = unsafe { memmap2::Mmap::map(&output) }.unwrap();
        assert_eq!(digest(&SHA256, &result[..size as usize]).as_ref(), p.new_partition_info.as_ref().unwrap().hash.as_ref().unwrap());
        assert_eq!(&result[size as usize..], &vec![0x55; 512]);
        eprintln!("Direct writer + SHA-256: {} bytes in {:.3}s", size, start.elapsed().as_secs_f64());
    }
}
