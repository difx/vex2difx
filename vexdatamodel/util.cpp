/***************************************************************************
 *   Copyright (C) 2012-2015 by Walter Brisken                             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
/*===========================================================================
 * SVN properties (DO NOT CHANGE)
 *
 * $Id$
 * $HeadURL: $
 * $LastChangedRevision$
 * $Author$
 * $LastChangedDate$
 *
 *==========================================================================*/

#include <set>
#include <string>
#include <cstdio>
#include <cstdlib>
#include "util.h"

/* Function to look through a file to make sure it is not DOS formatted */
int checkCRLF(const char *filename)
{
	static std::set<std::string> processedFiles;
	const int bufferSize = 1024;
	const char cr = 0x0d;
	FILE *in;
	char buffer[bufferSize];
	int n;

	if(processedFiles.find(filename) == processedFiles.end())
	{
		printf("Checking: %s\n", filename);
		processedFiles.insert(filename);

		in = fopen(filename, "rb");
		if(!in)
		{
			fprintf(stderr, "Error: cannot open %s\n", filename);

			return -1;
		}

		for(;;)
		{
			n = fread(buffer, 1, bufferSize, in);
			if(n < 1)
			{
				break;
			}

			for(int i = 0; i < n; ++i)
			{
				if(buffer[i] == cr)
				{
					fprintf(stderr, "Error: %s appears to be in DOS format.  Please run dos2unix or equivalent and try again.\n", filename);

					fclose(in);

					return -1;
				}
			}
		}

		fclose(in);
	}

	return 0;
}

/* round to nearest second */
double roundSeconds(double mjd)
{
	int intmjd, intsec;

	intmjd = static_cast<int>(mjd);
	intsec = static_cast<int>((mjd - intmjd)*86400.0 + 0.5);

	return intmjd + intsec/86400.0;
}

/* check if an integer is a power of 2 */
bool isPowerOf2(uint32_t n)
{
	if(!(n & (n - 1))) 
	{
		return true;
	}

	return false; // also true for zero but this shouldn't concern us
}

// round up to the next power of two
// There must be a more elegant solution!
uint32_t nextPowerOf2(uint32_t x)
{
	// This function returns the next higher power of 2 for some integer input.
	// If x is already a power of 2 value, it returns x.
	// If x==0, it returns 0;
	// See http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
	--x;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	++x;
	return x;

	// Original next2 code was for int x input, then:
	// int n=0; 
	// int m=0;
	
	// for(int i=0; i < 31; ++i)
	// {
	// 	if(x & (1 << i))
	// 	{
	// 		++n;
	// 		m = i;
	// 	}
	// }

	// if(n < 2)
	// {
	// 	return x;
	// }
	// else
	// {
	// 	return 2<<m;
	// }
}

/* Modified from http://www-graphics.stanford.edu/~seander/bithacks.html */
int intlog2(uint32_t v)
{
	const uint32_t b[] = {0x2, 0xC, 0xF0, 0xFF00, 0xFFFF0000};
	const uint32_t S[] = {1, 2, 4, 8, 16};
	uint32_t r = 0; // result of log2(v) will go here

	for(int i = 4; i >= 0; --i) 
	{
		if(v & b[i])
		{
			v >>= S[i];
			r |= S[i];
		} 
	}

	return r;
}

char swapPolarizationCode(char pol)
{
	switch(pol)
	{
	case 'R':
		return 'L';
	case 'L':
		return 'R';
	case 'X':
		return 'Y';
	case 'Y':
		return 'X';
	default:
		fprintf(stderr, "Error: unknown polarization: %c\n", pol);

		exit(EXIT_FAILURE);
	}
}

