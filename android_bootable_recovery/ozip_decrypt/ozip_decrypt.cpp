/*
	Copyright 2020 Mauronofrio

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	GNU General Public License <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <string.h>
#include <vector>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#define _FILE_OFFSET_BITS 64
//extern "C" __int64 __cdecl _ftelli64(FILE*);

using namespace std;
typedef std::vector<unsigned char> u_string;

int decrypt(unsigned char* ciphertext, int ciphertext_len, unsigned char* key,
	unsigned char* iv, unsigned char* plaintext)
{
	EVP_CIPHER_CTX* ctx;
	int len;
	int plaintext_len;
	ctx = EVP_CIPHER_CTX_new();
	EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, iv);
	EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len);
	plaintext_len = len;
	EVP_DecryptFinal_ex(ctx, plaintext + len, &len);
	plaintext_len += len;
	EVP_CIPHER_CTX_free(ctx);
	return plaintext_len;
}

std::string hexToASCII(string hex)
{
	int len = hex.length();
	std::string newString;
	for (int i = 0; i < len; i += 2)
	{
		string byte = hex.substr(i, 2);
		char chr = (char)(int)strtol(byte.c_str(), nullptr, 16);
		newString.push_back(chr);
	}
	return newString;
}

bool testkey(const char* keyf, const char* path) {
	std::string keyStr = hexToASCII(keyf);
	u_string key(keyStr.begin(), keyStr.end());
	int data[17];
	FILE* fps = fopen(path, "rb");
	fseek(fps, 4176, SEEK_SET);
	fread(data, sizeof(char), 16, fps);
	fclose(fps);
	u_string udata;
	unsigned char* dataPtr = (unsigned char*)data;
	for(int i = 0; i < 16; i++) {
		udata.push_back(dataPtr[i]);
	}
	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	EVP_CIPHER_CTX_init(ctx);
	EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key.data(), NULL);
	EVP_CIPHER_CTX_set_padding(ctx, false);
	unsigned char buffer[1024], * pointer = buffer;
	int outlen;
	EVP_DecryptUpdate(ctx, pointer, &outlen, udata.data(), udata.size());
	pointer += outlen;
	EVP_DecryptFinal_ex(ctx, pointer, &outlen);
	pointer += outlen;
	EVP_CIPHER_CTX_free(ctx);
	u_string test(buffer, buffer + (pointer - buffer));
	u_string checktest(test.begin(), test.begin() + 4);
	u_string pk0304 = {0x50, 0x4B, 0x03, 0x04};
	u_string andr = {0x41, 0x4E, 0x44, 0x52};
	if (checktest == pk0304 || checktest == andr) {
		return true;
	}
	return false;
}

int main(int argc, char* argv[])
{

	if (argc != 3)
	{
		printf("Usage: ozipdecrypt key [*.ozip]\n");
		return 0;
	}
	const char* key = argv[1];
	const char* path = argv[2];
	FILE* fp = fopen(path, "rb");
	char magic[13];
	fgets(magic, sizeof(magic), fp);
	string temp(path);
	temp = (temp.substr(0, temp.size() - 5)).append(".zip");
	const char* destpath= temp.c_str();
	if (strcmp(magic, "OPPOENCRYPT!") != 0)
	{
		printf("This is not an .ozip file!\n");
		fclose(fp);
		int rencheck = rename(path, destpath);
		if (rencheck == 0) {
			printf("Renamed .ozip file in .zip file\n");
		}
		else
		{
			printf("Unable to rename .ozip file in .zip file\n");
		}
		return 0;
	}
	if (testkey(key, path) == false)
	{
		printf("Key is not good!\n");
		fclose(fp);
		return 0;
	}
	else {
		printf("Key is good!\n");
	}
	FILE* fp2 = fopen(destpath, "wb");
	fseek(fp, 0L, SEEK_END);
	unsigned long int sizetot = ftello(fp);
	fseek(fp, 4176, SEEK_SET);
	int bdata[16384];
	unsigned long int sizeseek;
	printf("Decrypting...\n");
	while (true)
	{
		unsigned char data[17];
		fread(data, sizeof(char), 16, fp);
		std::string keyStr = hexToASCII(key);
		decrypt(data, sizeof(data), (unsigned char*)keyStr.c_str(), NULL, data);
		fwrite(data, sizeof(char), 16, fp2);
		sizeseek = ftello(fp);
		if ((sizetot - sizeseek) <= 16384) {
			fread(bdata, sizeof(char), (sizetot - sizeseek), fp);
			fwrite(bdata, sizeof(char), (sizetot - sizeseek), fp2);
			break;
		}
		else
		{
			fread(bdata, sizeof(char), 16384, fp);
			fwrite(bdata, sizeof(char), 16384, fp2);
		}
	}
	printf("File succesfully decrypted, saved in %s\n", destpath);
	fclose(fp2);
	fclose(fp);
	return 0;
}

