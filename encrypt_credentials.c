#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

/*
This program encrypt the credentials that is used to verify that
a software communicating with the Mellanox device is authorized
to manage crypto resources.
*/

void handleErrors(void)
{
	ERR_print_errors_fp(stderr);
	abort();
}

int encrypt(unsigned char *plaintext, int plaintext_len, unsigned char *kek,
		            unsigned char *iv, unsigned char *ciphertext)
{
	EVP_CIPHER_CTX *ctx;

	int len;

	int ciphertext_len;

	/* Create and initialise the context */
	if (!(ctx = EVP_CIPHER_CTX_new()))
		handleErrors();

	EVP_CIPHER_CTX_set_flags(ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);

	if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_128_wrap(), NULL, kek, iv))
		handleErrors();
	if (1 !=
	    EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len))
		handleErrors();
	ciphertext_len = len;

	if (1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len))
		handleErrors();
	ciphertext_len += len;

	/* Clean up */
	EVP_CIPHER_CTX_free(ctx);

	return ciphertext_len;
}


int main (int argc , char** argv)
{
	if(argc != 4){
		fprintf(stderr, "The application must get three parameters\n");
		exit(1);
	}
	/* A 128 bit kek */
	unsigned char kek[16] = {};
	unsigned char iv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
	unsigned char credentials[40] = {};
	unsigned char ciphertext[128] = {};
	int ciphertext_len;
	char * line = NULL;
	size_t len = 0;
	ssize_t read;
	int index = 0;
	char* eptr;
	int i;
	FILE* creds_file = NULL;
	FILE* kek_file = NULL;
	FILE* encrypted_credentials = NULL;

	creds_file = fopen(argv[1], "r");

	if(creds_file == NULL) {
		fprintf(stderr, "Couldn't open the credentials file\n");
		exit(1);
	}

	kek_file = fopen(argv[2], "r");

	if(kek_file == NULL) {
		fprintf(stderr, "Couldn't open key encryption key file\n");
		fclose(creds_file);
		exit(1);
	}

	while((read = getline(&line, &len, creds_file)) != -1) {

		if(index >= sizeof(credentials)) {
			fprintf(stderr, "Invalid credentials file\n");
			fclose(creds_file);
			fclose(kek_file);
			exit(1);
		}

		credentials[index] = strtol(line, &eptr, 16);
		index++;
	}

	fclose(creds_file);

	line = NULL;
	len = 0;
	index = 0;

	while((read = getline(&line, &len, kek_file)) != -1) {

		if(index >= sizeof(kek)) {
			fprintf(stderr, "Invalid key encryption key file\n");
			fclose(kek_file);
			exit(1);
		}

		kek[index] = strtol(line, &eptr, 16);
		index++;
	}

	fclose(kek_file);

	ciphertext_len =
		encrypt(credentials, sizeof(credentials), kek, iv, ciphertext);

	encrypted_credentials = fopen(argv[3], "w");

	if(encrypted_credentials == NULL) {
		fprintf(stderr, "Couldn't open the encrypted credentials file\n");
		exit(1);
	}

	for(i = 0; i < ciphertext_len; i++)
		fprintf(encrypted_credentials, "0x%02x\n", ciphertext[i]);

	fclose(encrypted_credentials);

	return 0;
}
