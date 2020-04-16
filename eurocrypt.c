/* hacktv - Analogue video transmitter for the HackRF                    */
/*=======================================================================*/
/* Copyright 2020 Philip Heron <phil@sanslogic.co.uk>                    */
/*                                                                       */
/* This program is free software: you can redistribute it and/or modify  */
/* it under the terms of the GNU General Public License as published by  */
/* the Free Software Foundation, either version 3 of the License, or     */
/* (at your option) any later version.                                   */
/*                                                                       */
/* This program is distributed in the hope that it will be useful,       */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of        */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         */
/* GNU General Public License for more details.                          */
/*                                                                       */
/* You should have received a copy of the GNU General Public License     */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "eurocrypt.h"

/* Data for EC controlled-access decoding (CTV) */
static ec_t ec_ctv = { 
	
	/* Key 0x08 */
	{ 0x84, 0x66, 0x30, 0xE4, 0xDA, 0xFA, 0x23 },
	
	/* General ECM data */
	{ 	/*           |-Channel ID-|| Key index */      
		0x90, 0x03, 0x00, 0x04, 0x38,
		
		 /* Control */
 		0xE0, 0x01, 0x00,
		
		/* Date, theme and level */
		0xE1, 0x04, 0x21, 0x65, 0xFF, 0x00,
		
		/* Even CW header */
		0xEA, 0x10,
		
		/* Encrypted even CW (random) */
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		
		/* Encrypted odd CW (random) */
		0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x01,
		
		/* Hash header */
		0xF0, 0x08,
		
		/* Hash data */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	}
};

/* Data for EC controlled-access decoding (TV Plus Holland) */
static ec_t ec_tvplus = { 
	
	/* Key 0x08 */
	{ 0x12, 0x06, 0x28, 0x3A, 0x4B, 0x1D, 0xE2 },
	
	/* General ECM data */
	{ 	/*           |-Channel ID-|| Key index */      
		0x90, 0x03, 0x00, 0x2c, 0x08,
		
		 /* Control */
 		0xE0, 0x01, 0x00,
		
		/* Date, theme and level */
		0xE1, 0x04, 0x21, 0x65, 0x04, 0x00,
		
		/* Even CW header */
		0xEA, 0x10,
		
		/* Encrypted even CW (random) */
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		
		/* Encrypted odd CW (random) */
		0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x01,
		
		/* Hash header */
		0xF0, 0x08,
		
		/* Hash data */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	}
};

/* Data for EC controlled-access decoding (TV1000) */
static ec_t ec_tv1000 = { 
	
	/* Key 0x0F */
	{ 0x36, 0xFA, 0xCD, 0x50, 0x85, 0x54, 0xDF },
	
	/* General ECM data */
	{ 	/*           |-Channel ID-|| Key index */      
		0x90, 0x03, 0x00, 0x04, 0x1F,
		
		 /* Control */
 		0xE0, 0x01, 0x00,
		
		/* Date, theme and level */
		0xE1, 0x04, 0x21, 0x65, 0x05, 0x04,
		
		/* Even CW header */
		0xEA, 0x10,
		
		/* Encrypted even CW (random) */
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		
		/* Encrypted odd CW (random) */
		0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x01,
		
		/* Hash header */
		0xF0, 0x08,
		
		/* Hash data */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	}
};

/* Data for EC controlled-access decoding (FilmNet) */
static ec_t ec_filmnet = { 
	
	/* Key 0x08 */
	{ 0x21, 0x12, 0x31, 0x35, 0x8A, 0xC3, 0x4F },
	
	/* General ECM data */
	{ 	/*           |-Channel ID-|| Key index */      
		0x90, 0x03, 0x00, 0x28, 0x08,
		
		 /* Control */
 		0xE0, 0x01, 0x00,
		
		/* Date, theme and level */
		0xE1, 0x04, 0x21, 0x15, 0x05, 0x00,
		
		/* Even CW header */
		0xEA, 0x10,
		
		/* Encrypted even CW (random) */
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		
		/* Encrypted odd CW (random) */
		0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x01,
		
		/* Hash header */
		0xF0, 0x08,
		
		/* Hash data */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	}
};

unsigned char EXP[E_DIM] = {
		32,  1,  2,  3,  4,  5,
		 4,  5,  6,  7,  8,  9,
		 8,  9, 10, 11, 12, 13,
		12, 13, 14, 15, 16, 17,
		16, 17, 18, 19, 20, 21,
		20, 21, 22, 23, 24, 25,
		24, 25, 26, 27, 28, 29,
		28, 29, 30, 31, 32,  1
};

unsigned char SB[S_BOXES][S_DIM] = {
		{ 0x0E, 0x00, 0x04, 0x0F, 0x0D, 0x07, 0x01, 0x04,
		  0x02, 0x0E, 0x0F, 0x02, 0x0B, 0x0D, 0x08, 0x01,
		  0x03, 0x0A, 0x0A, 0x06, 0x06, 0x0C, 0x0C, 0x0B,
		  0x05, 0x09, 0x09, 0x05, 0x00, 0x03, 0x07, 0x08,
		  0x04, 0x0F, 0x01, 0x0C, 0x0E, 0x08, 0x08, 0x02,
		  0x0D, 0x04, 0x06, 0x09, 0x02, 0x01, 0x0B, 0x07,
		  0x0F, 0x05, 0x0C, 0x0B, 0x09, 0x03, 0x07, 0x0E,
		  0x03, 0x0A, 0x0A, 0x00, 0x05, 0x06, 0x00, 0x0D
		},
		{ 0x0F, 0x03, 0x01, 0x0D, 0x08, 0x04, 0x0E, 0x07,
		  0x06, 0x0F, 0x0B, 0x02, 0x03, 0x08, 0x04, 0x0E,
		  0x09, 0x0C, 0x07, 0x00, 0x02, 0x01, 0x0D, 0x0A,
		  0x0C, 0x06, 0x00, 0x09, 0x05, 0x0B, 0x0A, 0x05,
		  0x00, 0x0D, 0x0E, 0x08, 0x07, 0x0A, 0x0B, 0x01,
		  0x0A, 0x03, 0x04, 0x0F, 0x0D, 0x04, 0x01, 0x02,
		  0x05, 0x0B, 0x08, 0x06, 0x0C, 0x07, 0x06, 0x0C,
		  0x09, 0x00, 0x03, 0x05, 0x02, 0x0E, 0x0F, 0x09
		},
		{ 0x0A, 0x0D, 0x00, 0x07, 0x09, 0x00, 0x0E, 0x09,
		  0x06, 0x03, 0x03, 0x04, 0x0F, 0x06, 0x05, 0x0A,
		  0x01, 0x02, 0x0D, 0x08, 0x0C, 0x05, 0x07, 0x0E,
		  0x0B, 0x0C, 0x04, 0x0B, 0x02, 0x0F, 0x08, 0x01,
		  0x0D, 0x01, 0x06, 0x0A, 0x04, 0x0D, 0x09, 0x00,
		  0x08, 0x06, 0x0F, 0x09, 0x03, 0x08, 0x00, 0x07,
		  0x0B, 0x04, 0x01, 0x0F, 0x02, 0x0E, 0x0C, 0x03,
		  0x05, 0x0B, 0x0A, 0x05, 0x0E, 0x02, 0x07, 0x0C
		},
		{ 0x07, 0x0D, 0x0D, 0x08, 0x0E, 0x0B, 0x03, 0x05,
		  0x00, 0x06, 0x06, 0x0F, 0x09, 0x00, 0x0A, 0x03,
		  0x01, 0x04, 0x02, 0x07, 0x08, 0x02, 0x05, 0x0C,
		  0x0B, 0x01, 0x0C, 0x0A, 0x04, 0x0E, 0x0F, 0x09,
		  0x0A, 0x03, 0x06, 0x0F, 0x09, 0x00, 0x00, 0x06,
		  0x0C, 0x0A, 0x0B, 0x01, 0x07, 0x0D, 0x0D, 0x08,
		  0x0F, 0x09, 0x01, 0x04, 0x03, 0x05, 0x0E, 0x0B,
		  0x05, 0x0C, 0x02, 0x07, 0x08, 0x02, 0x04, 0x0E
		},
		{ 0x02, 0x0E, 0x0C, 0x0B, 0x04, 0x02, 0x01, 0x0C,
		  0x07, 0x04, 0x0A, 0x07, 0x0B, 0x0D, 0x06, 0x01,
		  0x08, 0x05, 0x05, 0x00, 0x03, 0x0F, 0x0F, 0x0A,
		  0x0D, 0x03, 0x00, 0x09, 0x0E, 0x08, 0x09, 0x06,
		  0x04, 0x0B, 0x02, 0x08, 0x01, 0x0C, 0x0B, 0x07,
		  0x0A, 0x01, 0x0D, 0x0E, 0x07, 0x02, 0x08, 0x0D,
		  0x0F, 0x06, 0x09, 0x0F, 0x0C, 0x00, 0x05, 0x09,
		  0x06, 0x0A, 0x03, 0x04, 0x00, 0x05, 0x0E, 0x03
		},
		{ 0x0C, 0x0A, 0x01, 0x0F, 0x0A, 0x04, 0x0F, 0x02,
		  0x09, 0x07, 0x02, 0x0C, 0x06, 0x09, 0x08, 0x05,
		  0x00, 0x06, 0x0D, 0x01, 0x03, 0x0D, 0x04, 0x0E,
		  0x0E, 0x00, 0x07, 0x0B, 0x05, 0x03, 0x0B, 0x08,
		  0x09, 0x04, 0x0E, 0x03, 0x0F, 0x02, 0x05, 0x0C,
		  0x02, 0x09, 0x08, 0x05, 0x0C, 0x0F, 0x03, 0x0A,
		  0x07, 0x0B, 0x00, 0x0E, 0x04, 0x01, 0x0A, 0x07,
		  0x01, 0x06, 0x0D, 0x00, 0x0B, 0x08, 0x06, 0x0D
		},
		{ 0x04, 0x0D, 0x0B, 0x00, 0x02, 0x0B, 0x0E, 0x07,
		  0x0F, 0x04, 0x00, 0x09, 0x08, 0x01, 0x0D, 0x0A,
		  0x03, 0x0E, 0x0C, 0x03, 0x09, 0x05, 0x07, 0x0C,
		  0x05, 0x02, 0x0A, 0x0F, 0x06, 0x08, 0x01, 0x06,
		  0x01, 0x06, 0x04, 0x0B, 0x0B, 0x0D, 0x0D, 0x08,
		  0x0C, 0x01, 0x03, 0x04, 0x07, 0x0A, 0x0E, 0x07,
		  0x0A, 0x09, 0x0F, 0x05, 0x06, 0x00, 0x08, 0x0F,
		  0x00, 0x0E, 0x05, 0x02, 0x09, 0x03, 0x02, 0x0C
		},
		{ 0x0D, 0x01, 0x02, 0x0F, 0x08, 0x0D, 0x04, 0x08,
		  0x06, 0x0A, 0x0F, 0x03, 0x0B, 0x07, 0x01, 0x04,
		  0x0A, 0x0C, 0x09, 0x05, 0x03, 0x06, 0x0E, 0x0B,
		  0x05, 0x00, 0x00, 0x0E, 0x0C, 0x09, 0x07, 0x02,
		  0x07, 0x02, 0x0B, 0x01, 0x04, 0x0E, 0x01, 0x07,
		  0x09, 0x04, 0x0C, 0x0A, 0x0E, 0x08, 0x02, 0x0D,
		  0x00, 0x0F, 0x06, 0x0C, 0x0A, 0x09, 0x0D, 0x00,
		  0x0F, 0x03, 0x03, 0x05, 0x05, 0x06, 0x08, 0x0B
		}
};

unsigned char	PERM[P_DIM] = {
	16,  7, 20, 21,
	29, 12, 28, 17,
	 1, 15, 23, 26,
	 5, 18, 31, 10,
	 2,  8, 24, 14,
	32, 27,  3,  9,
	19, 13, 30,  6,
	22, 11,  4, 25
};

unsigned char	PC2[PC2_DIM] = {
	14, 17, 11, 24,  1,  5,
	 3, 28, 15,  6, 21, 10,
	23, 19, 12,  4, 26,  8,
	16,  7, 27, 20, 13,  2,
	41, 52, 31, 37, 47, 55,
	30, 40, 51, 45, 33, 48,
	44, 49, 39, 56, 34, 53,
	46, 42, 50, 36, 29, 32
};

unsigned char LSHIFT[LS_DIM] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

unsigned long _permute_f(unsigned long R, unsigned char *K)
{
	int i, k;
	unsigned long s = 0, result = 0;

	for(i = 0, k = 0; i < 8; i++)
	{
		int j;
		char v = 0;

		/* The expansion E (R1) */
		for(j = 0; j < 6; j++, k++)
		{
			v |= (R >> (32 - EXP[k]) & 1) << (5 - j);
		}
		
		/* Create R2 */
		v ^= K[i];
		
		/* The S-boxes */
		s |= (unsigned long)SB[i][v] << (28 - 4 * i);
	}
	
	/* The permutation P (R3) */
	for(i=0; i<32; i++)
	{
		result |= (s >> (32 - PERM[i]) & 1) << (31 - i);
	}
	
	return result;
}

void eurocrypt_m(unsigned char *in, unsigned char *out, const unsigned char *key, int hash)
{
	int i;
	unsigned long R, L, C, D, S;
	
	memcpy(out, in, 8);
	
	/* Key preparation. Split key into two halves. */
	C = (unsigned long)key[0] << 20
		^ (unsigned long)key[1] << 12
		^ (unsigned long)key[2] <<	4
		^ (unsigned long)(key[3] >> 4);

	D = (unsigned long)(key[3] & 0x0f) << 24
		^ (unsigned long)key[4] << 16
		^ (unsigned long)key[5] << 8
		^ (unsigned long)key[6];

	/* Control word preparation. Split CW into two halves. */
	L = (unsigned long)in[0] << 24
		^ (unsigned long)in[1] << 16
		^ (unsigned long)in[2] << 8
		^ (unsigned long)in[3];

	R = (unsigned long)in[4] << 24
		^ (unsigned long)in[5] << 16
		^ (unsigned long)in[6] << 8
		^ (unsigned long)in[7];
		
	/* 16 iterations */
	for(i = 0; i < 16; i++)
	{
		unsigned long R3;
		unsigned char K[8];
		int j, k;
		
		/* Key shift rotation */
		for(j = 0; j < LSHIFT[i]; j++ )
		{
			C = (C << 1 ^ C >> 27) & 0xfffffffL;
			D = (D << 1 ^ D >> 27) & 0xfffffffL;
		}
		
		/* Key expansion */
		for(j = 0, k = 0; j < 8; j++ )
		{
			int t;
			K[j]=0;
			
			for(t = 0; t < 6; t++, k++)
			{
			if(PC2[k] < 29)
			{
				K[j] |= (C >> (28 - PC2[k]) & 1) << (5 - t);
			}
			else
			{
				K[j] |= (D >> (56 - PC2[k]) & 1) << (5 - t);
			}
		}
		}
		
		/* One decryption round */
		S	= _permute_f(R, K);
		
		/* Swap first two bytes if it's a hash routine */
		if(hash)
		{
			S = (S >> 8 & 0xFF0000L) | (S << 8 & 0xFF000000L) | (S & 0x0000FFFFL);
		}
		
		/* Rotate halves around */
		R3 = L ^ S;
		L = R;
		R = R3;
	}
	
	/* Put everything together */
	out[0] = R >> 24;
	out[1] = R >> 16;
	out[2] = R >> 8;
	out[3] = R;
	out[4] = L >> 24;
	out[5] = L >> 16;
	out[6] = L >> 8;
	out[7] = L;

}

void generate_ecm(ec_t *e, int cafcnt)
{
	int j;
	unsigned char hash[8];
	
	/* Generate new encrypted CW - either even or odd at one time */
	if(cafcnt) 
	{
		for(j = 16; j < 24; j++) e->data[j] = rand() + 0xFF;
	}
	else
	{
		for(j = 24; j < 32; j++) e->data[j] = rand() + 0xFF;
	}
	
	/* Decrypt even control word */
	eurocrypt_m(e->data + 16, e->decevencw, e->key, ECM);
	
	/* Decrypt odd control word */
	eurocrypt_m(e->data + 24, e->decoddcw, e->key, ECM);
	
	/* Zeroise hash */
	for(j = 0; j < 8; j++) hash[j] = 0;
	
	/* Iterate through data */
	for(j = 0; j < 27; j++)
	{
		hash[(j % 8)] ^= e->data[j + 5];
		
		if(j % 8 == 7)
		{
			eurocrypt_m(hash, hash, e->key, HASH);
		}
	}
	
	/* Final interation */
	eurocrypt_m(hash, hash, e->key, HASH);
	
	/* Copy hash to main data area */
	memcpy(e->data + 34, hash, 8);
}

void eurocrypt_init(mac_t *mac, char *mode)
{
	time_t t;
	srand((unsigned) time(&t));
	
	mac->ec = malloc(sizeof(ec_t));

	if(strcmp(mode, "filmnet") == 0)
	{
		mac->ec = &ec_filmnet;
	}
	else if(strcmp(mode, "tv1000") == 0)
	{
		mac->ec = &ec_tv1000;
	}
	else if(strcmp(mode, "tvplus") == 0)
	{
		mac->ec = &ec_tvplus;
	}
	else if(strcmp(mode, "ctv") == 0)
	{
		mac->ec = &ec_ctv;
	}
	
	/* Generate initial even and odd ECMs */
	generate_ecm(mac->ec, 0);
	generate_ecm(mac->ec, 1);
}
