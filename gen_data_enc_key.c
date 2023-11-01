#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

/*
This program generates a data encryption key that
is used to encrypt data that is sent in perftest applications
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
	if(argc != 3){
		fprintf(stderr, "The application should get 3 parameters\n");
		exit(1);
	}
	/* A 128 bit kek */
	unsigned char kek[16] = {};

	/* A 64 bit IV */
	unsigned char iv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};

	unsigned char dek[32] = {};
	unsigned char ciphertext[128] = {};
	int ciphertext_len;

	char * line = NULL;
	size_t len = 0;
	ssize_t read;
	int index = 0;
	char* eptr;
	int i;
	FILE* kek_file = NULL;
	FILE* dek_file = NULL;

	srand(time(NULL));

	//random
	for(i = 0; i < 32; i++){
		dek[i]=(rand()%256);
	}

	kek_file = fopen(argv[1] , "r");

	if(kek_file == NULL){
		fprintf(stderr, "Couldn't open key encryption key file\n");
		exit(1);
	}

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
		encrypt(dek, sizeof(dek), kek, iv, ciphertext);

	dek_file = fopen(argv[2] , "w");

	if(dek_file == NULL){
		fprintf(stderr, "Couldn't open data encryption key file\n");
		exit(1);
	}

	for(i = 0; i < ciphertext_len; i++)
		fprintf(dek_file, "0x%02x\n", ciphertext[i]);

	fclose(dek_file);

	return 0;
}
