// AERA integration additions, Copyright 2026 AERA Recovery Project contributors.
// SPDX-License-Identifier: Apache-2.0
use indicatif::ProgressBar;
use std::sync::{Arc, atomic::{AtomicBool, Ordering}};
use std::thread::JoinHandle;
use std::time::Duration;

// Poll atomic counters, not stdout from every operation worker.
pub(super) struct Reporter {
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}
impl Reporter {
    pub fn start(bars: Vec<ProgressBar>, total: u64) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let token = stop.clone();
        let worker = std::thread::spawn(move || {
            while !token.load(Ordering::Acquire) {
                let done = bars.iter().map(ProgressBar::position).sum::<u64>().min(total);
                println!("AERA_PROGRESS {} {}", done, total);
                std::thread::sleep(Duration::from_millis(100));
            }
        });
        Self { stop, worker: Some(worker) }
    }
}
impl Drop for Reporter {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() { let _ = worker.join(); }
    }
}
