/***************************************************************************
 *   Copyright (C) 2009 by Walter Brisken                                  *
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
 * $HeadURL$
 * $LastChangedRevision$
 * $Author$
 * $LastChangedDate$
 *
 *==========================================================================*/

#include <vector>
#include <set>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/time.h>
#include <sys/stat.h>
#include <difxio/difx_input.h>
#include "vextables.h"
#include "corrparams.h"
#include "vexload.h"

const string program("vex2difx");
const string version("1.0");
const string verdate("20090902");
const string author("Walter Brisken");


double current_mjd()
{
	struct timeval t;
	gettimeofday(&t, 0);
	return MJD_UNIX0 + t.tv_sec/SEC_DAY + t.tv_usec/MUSEC_DAY;
}

// A is assumed to be the first scan in time order
bool areScansCompatible(const VexScan *A, const VexScan *B, const CorrParams *P)
{
	if((B->mjdStart < A->mjdStop) ||
	   (B->mjdStart > A->mjdStop + P->maxGap))
	{
		return false;
	}
	if(P->singleScan)
	{
		return false;
	}
	if(P->singleSetup && A->modeName != B->modeName)
	{
		return false;
	}
	
	return true;
}

void genJobGroups(vector<VexJobGroup> &JGs, const VexData *V, const CorrParams *P, int verbose)
{
	list<string> scans;
	V->getScanList(scans);

	while(!scans.empty())
	{
		JGs.push_back(VexJobGroup());
		VexJobGroup &JG = JGs.back();
		JG.scans.push_back(scans.front());
		JG.setTimeRange( *(V->getScan(scans.front())) );
		scans.pop_front();

		const VexScan *scan1 = V->getScan(JG.scans.back());
		const CorrSetup *corrSetup1 = P->getCorrSetup(scan1->corrSetupName);

		for(list<string>::iterator it = scans.begin(); it != scans.end();)
		{
			const VexScan *scan2 = V->getScan(*it);
			const CorrSetup *corrSetup2 = P->getCorrSetup(scan2->corrSetupName);

			// Skip any scans that don't overlap with .v2d mjdStart and mjdStop
			if(P->overlap(*scan2) <= 0.0)
			{
				continue;
			}

			// FIXME -- verify modes are compatible
			if(areCorrSetupsCompatible(corrSetup1, corrSetup2, P) &&
			   areScansCompatible(scan1, scan2, P))
			{
				JG.logicalOr(*scan2);	// expand jobGroup time to include this scan
				JG.scans.push_back(*it);
				it = scans.erase(it);
				scan1 = scan2;
				corrSetup1 = corrSetup2;
			}
			else
			{	
				it++;
			}
		}
	}

	const list<VexEvent> *events = V->getEvents();
	for(vector<VexJobGroup>::iterator jg = JGs.begin(); jg != JGs.end(); jg++)
	{
		jg->genEvents(*events);
		jg->logicalAnd(*P);		// possibly shrink job group to requested range
	}
}

class MediaChange : public VexInterval
{
public:
	MediaChange(string A, double start, double stop) : VexInterval(start, stop), ant(A) {}

	string ant;
};

int nGap(const list<MediaChange> &m, double mjd)
{
	list<MediaChange>::const_iterator it;
	int n=0;

	for(it = m.begin(); it != m.end(); it++)
	{
		if(mjd >= it->mjdStart && mjd <= it->mjdStop)
		{
			n++;
		}
	}

	return n;
}

void genJobs(vector<VexJob> &Js, const VexJobGroup &JG, VexData *V, const CorrParams *P, int verbose)
{
	list<VexEvent>::const_iterator e;
	list<double>::const_iterator t;
	list<MediaChange>::iterator c;
	map<string,double> recordStop;
	map<double,int> usage;
	map<double,int> clockBreaks;
	int nClockBreaks = 0;
	list<MediaChange> changes;
	list<double> times;
	list<double> breaks;
	double mjdLast = -1.0;
	int score, scoreBest;
	double mjdBest = 0.0;
	double start;
	int nAnt;
	int nLoop = 0;

	// first initialize recordStop and usage
	for(e = JG.events.begin(); e != JG.events.end(); e++)
	{
		if(e->eventType == VexEvent::RECORD_START)
		{
			recordStop[e->name] = -1.0;
		}

		usage[e->mjd] = 0;
		clockBreaks[e->mjd] = 0;
	}
	nAnt = recordStop.size();

	// populate changes, times, and usage
	for(e = JG.events.begin(); e != JG.events.end(); e++)
	{
		if(mjdLast > 0.0 && e->mjd > mjdLast)
		{
			usage[e->mjd] = usage[mjdLast];
			mjdLast = e->mjd;
			times.push_back(e->mjd);
		}
		else if(mjdLast < 0.0)
		{
			usage[e->mjd] = 0;
			mjdLast = e->mjd;
			times.push_back(e->mjd);
		}

		if(e->eventType == VexEvent::RECORD_START)
		{
			if(recordStop[e->name] > 0.0)
			{
				changes.push_back(MediaChange(e->name, recordStop[e->name], e->mjd));
				if(verbose > 0)
				{
					cout << "Media change: " << e->name << " " << 
						(VexInterval)(changes.back()) << endl;
				}
			}
		}
		else if(e->eventType == VexEvent::RECORD_STOP)
		{
			recordStop[e->name] = e->mjd;
		}
		else if(e->eventType == VexEvent::ANT_SCAN_START)
		{
			usage[e->mjd]++;
		}
		else if(e->eventType == VexEvent::ANT_SCAN_STOP)
		{
			usage[e->mjd]--;
		}
		else if(e->eventType == VexEvent::CLOCK_BREAK ||
			e->eventType == VexEvent::LEAP_SECOND ||
			e->eventType == VexEvent::MANUAL_BREAK)
		{
			clockBreaks[e->mjd]++;
			nClockBreaks++;
		}
	}

	// now go through and set breakpoints
	while(!changes.empty() || nClockBreaks > 0)
	{
		nLoop++;
		if(nLoop > 100000) // There is clearly a problem converging!
		{
			cerr << "Developer error! -- jobs not converging after " << nLoop << " tries.\n" << endl;
			exit(0);
		}

		// look for break with highest score
		// Try as hard as possible to minimize number of breaks
		scoreBest = -1;
		for(t = times.begin(); t != times.end(); t++)
		{
			score = nGap(changes, *t) * (nAnt-usage[*t]+1) + 100*clockBreaks[*t];
			if(score > scoreBest)
			{
				scoreBest = score;
				mjdBest = *t;
			}
		}

		breaks.push_back(mjdBest);
		nClockBreaks -= clockBreaks[mjdBest];
		clockBreaks[mjdBest] = 0;

		// find modules that change in the new gap
		for(c = changes.begin(); c != changes.end();)
		{
			if(c->mjdStart <= mjdBest && c->mjdStop >= mjdBest)
			{
				c = changes.erase(c);
			}
			else
			{
				c++;
			}
		}
	}
	breaks.sort();

	// Add a break at end so num breaks = num jobs
	breaks.push_back(JG.mjdStop);

	// form jobs
	start = JG.mjdStart;
	for(t = breaks.begin(); t != breaks.end(); t++)
	{
		VexInterval jobTimeRange(start, *t);
		if(jobTimeRange.duration() > P->minLength)
		{
			JG.createJobs(Js, jobTimeRange, V, P->maxLength, P->maxSize);
		}
		else
		{
			cerr << "Warning: skipping short job of " << (jobTimeRange.duration()*86400.0) << " seconds duration." << endl;
		}
		start = *t;
	}
}

void makeJobs(vector<VexJob>& J, VexData *V, const CorrParams *P, int verbose)
{
	vector<VexJobGroup> JG;
	vector<VexJob>::iterator j;
	int k;

	// Do splitting of jobs
	genJobGroups(JG, V, P, verbose);

	if(verbose > 0)
	{
		cout << JG.size() << " job groups created:" << endl;
		for(unsigned int i = 0; i < JG.size(); i++)
		{
			cout << "  " << JG[i];
		}
	}

	for(unsigned int i = 0; i < JG.size(); i++)
	{
		genJobs(J, JG[i], V, P, verbose);
	}

	// Finalize all the new job structures
	for(j = J.begin(), k = P->startSeries; j != J.end(); j++, k++)
	{
		ostringstream name;
		j->jobSeries = P->jobSeries;
		j->jobId = k;

		// note -- this is an internal name only, not the job prefix that 
		// becomes part of the filenames
		name << j->jobSeries << "_" << j->jobId;

		V->addEvent(j->mjdStart, VexEvent::JOB_START, name.str());
		V->addEvent(j->mjdStop,  VexEvent::JOB_STOP,  name.str());
		j->assignVSNs(*V);
	}
}

DifxJob *makeDifxJob(string directory, const VexJob& J, int nAntenna, const string& obsCode, int *n, int nDigit, char ext)
{
	DifxJob *job;
	const char *difxVer;
	char format[16];

	*n = 1;
	job = newDifxJobArray(*n);
	difxVer = getenv("DIFX_VERSION");
	if(difxVer)
	{
		strcpy(job->difxVersion, difxVer);
	}
	job->jobStart = J.mjdStart;
	job->jobStop  = J.mjdStop;
	job->mjdStart = J.mjdStart;
	job->duration = trunc((J.mjdStop - J.mjdStart) * 86400.0 + 0.001);
	job->modelInc = 1;
	job->jobId    = J.jobId;
	job->subarrayId = 0;
	strncpy(job->obsCode, obsCode.c_str(), 8);
	job->obsCode[7] = 0;
	strcpy(job->taperFunction, "UNIFORM");
	job->polyOrder = 5;
	job->polyInterval = 120;
	job->aberCorr = AberCorrExact;
	job->activeBaselines = nAntenna;
	job->activeBaselines = nAntenna*(nAntenna-1)/2;
	job->dutyCycle = J.dutyCycle;

	// The following line defines the format of the job filenames
	sprintf(format, "%%s/%%s_%%0%dd%%c", nDigit);

	sprintf(job->fileBase, format, directory.c_str(), J.jobSeries.c_str(), J.jobId, ext);

	return job;
}

DifxAntenna *makeDifxAntennas(const VexJob& J, const VexData *V, const CorrParams *P, int *n, vector<string>& antList)
{
	const VexAntenna *ant;
	DifxAntenna *A;
	int i;
	double offset, rate, mjd;
	map<string,string>::const_iterator a;

	mjd = 0.5*(V->obsStart() + V->obsStop());

	*n = J.vsns.size();

	antList.clear();

	A = newDifxAntennaArray(*n);
	for(i = 0, a = J.vsns.begin(); a != J.vsns.end(); i++, a++)
	{
		ant = V->getAntenna(a->first);
		strcpy(A[i].name, a->first.c_str());
		int nFile = ant->basebandFiles.size();
		if(nFile > 0)
		{
			int count = 0;

			for(int j = 0; j < nFile; j++)
			{
				if(J.overlap(ant->basebandFiles[j]) > 0.0)
				{
					count++;
				}
			}

			allocateDifxAntennaFiles(A+i, count);
			
			count = 0;

			for(int j = 0; j < nFile; j++)
			{
				if(J.overlap(ant->basebandFiles[j]) > 0.0)
				{
					A[i].file[count] = 
						strdup(ant->basebandFiles[j].filename.c_str());
					count++;
				}
			}
		}
		else
		{
			strcpy(A[i].vsn, a->second.c_str());
		}
		A[i].X = ant->x + ant->dx*(mjd-ant->posEpoch)*86400.0;
		A[i].Y = ant->y + ant->dy*(mjd-ant->posEpoch)*86400.0;
		A[i].Z = ant->z + ant->dz*(mjd-ant->posEpoch)*86400.0;
		strcpy(A[i].mount, ant->axisType.c_str());
		ant->getClock(J.mjdStart, offset, rate);
		A[i].delay = offset*1.0e6;	// convert to us from sec
		A[i].rate  = rate*1.0e6;	// convert to us/sec from sec/sec
		A[i].offset[0] = ant->axisOffset;
		A[i].offset[1] = 0.0;
		A[i].offset[2] = 0.0;

		/* override with antenna setup values? */
		const AntennaSetup *antSetup = P->getAntennaSetup(a->first);
		if(antSetup)
		{
			if(fabs(antSetup->X) > 1.0)
			{
				A[i].X = antSetup->X;
			}
			if(fabs(antSetup->Y) > 1.0)
			{
				A[i].Y = antSetup->Y;
			}
			if(fabs(antSetup->Z) > 1.0)
			{
				A[i].Z = antSetup->Z;
			}
			if(antSetup->difxName.size() > 0)
			{
				strcpy(A[i].name, antSetup->difxName.c_str());
			}
			A[i].networkPort = antSetup->networkPort;
			A[i].windowSize  = antSetup->windowSize;
		}

		antList.push_back(a->first);
		strcpy(A[i].shelf, P->getShelf(a->second));
	}

	return A;
}

DifxDatastream *makeDifxDatastreams(const VexJob& J, const VexData *V, int nSet, int *n)
{
	DifxDatastream *D;
	int i;
	
	*n = J.vsns.size() * nSet;
	D = newDifxDatastreamArray(*n);
	for(i = 0; i < *n; i++)
	{
		D[i].antennaId = i % J.vsns.size();
		D[i].tSys = 0.0;
	}

	return D;
}

// round up to the next power of two
int next2(int x)
{
	int n=0; 
	int m=0;
	
	for(int i=0; i < 31; i++)
	{
		if(x & (1 << i))
		{
			n++;
			m = i;
		}
	}

	if(n < 2)
	{
		return x;
	}
	else
	{
		return 2<<m;
	}
}

class freq
{
public:
	freq(double f=0.0, double b=0.0, char s=' ') : fq(f), bw(b), sideBand(s) {};
	double fq;
	double bw;
	char sideBand;
};

int getFreqId(vector<freq>& freqs, double fq, double bw, char sb)
{
	for(unsigned int i = 0; i < freqs.size(); i++)
	{
		if(fq == freqs[i].fq &&
		   bw == freqs[i].bw &&
		   sb == freqs[i].sideBand)
		{
			return i;
		}
	}

	freqs.push_back(freq(fq, bw, sb));

	return freqs.size() - 1;
}

int getBand(vector<pair<int,int> >& bandMap, int fqId)
{
	vector<pair<int,int> >::iterator it;
	int i;

	for(i = 0, it = bandMap.begin(); it != bandMap.end(); i++, it++)
	{
		if(it->first == fqId)
		{
			it->second++;
			return i;
		}
	}

	bandMap.push_back(pair<int,int>(fqId, 1));

	return bandMap.size() - 1;
}
	
static int setFormat(DifxInput *D, int dsId, vector<freq>& freqs, const VexMode *mode, const string &antName)
{
	vector<pair<int,int> > bandMap;

	int antId = D->datastream[dsId].antennaId;
	if(antId < 0 || antId >= D->nAntenna)
	{
		cerr << "Error: setFormat: antId=" << antId << " while nAntenna=" << D->nAntenna << endl;
		exit(0);
	}
	const VexFormat &format = mode->getFormat(antName);
	int n2 = next2(format.nRecordChan);

	if(format.format == string("VLBA1_1"))
	{
		strcpy(D->datastream[dsId].dataFormat, "VLBA");
		D->datastream[dsId].dataFrameSize = 2520*format.nBit*n2;
	}
	else if(format.format == string("VLBA1_2"))
	{
		strcpy(D->datastream[dsId].dataFormat, "VLBA");
		D->datastream[dsId].dataFrameSize = 5040*format.nBit*n2;
	}
	else if(format.format == string("VLBA1_4"))
	{
		strcpy(D->datastream[dsId].dataFormat, "VLBA");
		D->datastream[dsId].dataFrameSize = 10080*format.nBit*n2;
	}
	else if(format.format == string("MKIV1_1"))
	{
		strcpy(D->datastream[dsId].dataFormat, "MKIV");
		D->datastream[dsId].dataFrameSize = 2500*format.nBit*n2;
	}
	else if(format.format == string("MKIV1_2"))
	{
		strcpy(D->datastream[dsId].dataFormat, "MKIV");
		D->datastream[dsId].dataFrameSize = 5000*format.nBit*n2;
	}
	else if(format.format == string("MKIV1_4"))
	{
		strcpy(D->datastream[dsId].dataFormat, "MKIV");
		D->datastream[dsId].dataFrameSize = 10000*format.nBit*n2;
	}
	else if(format.format == string("MARK5B"))
	{
		strcpy(D->datastream[dsId].dataFormat, "MARK5B");
		D->datastream[dsId].dataFrameSize = 10016;
	}
	else if(format.format == string("S2"))
	{
		strcpy(D->datastream[dsId].dataFormat, "LBAVSOP");
		D->datastream[dsId].dataFrameSize = 4096 + 10*format.nBit*n2*(int)(mode->sampRate+0.5)/8;
		cerr << "Warning: S2 data can be in LBAVSOP or LBASTD format - defaulting to LBAVSOP!!" << endl;
	}
	else if(format.format == string("LBAVSOP"))
	{
		strcpy(D->datastream[dsId].dataFormat, "LBAVSOP");
		D->datastream[dsId].dataFrameSize = 4096 + 10*format.nBit*n2*(int)(mode->sampRate+0.5)/8;
	}
	else if(format.format == string("LBASTD"))
	{
		strcpy(D->datastream[dsId].dataFormat, "LBASTD");
		D->datastream[dsId].dataFrameSize = 4096 + 10*format.nBit*n2*(int)(mode->sampRate+0.5)/8;
	}
	else
	{
		cerr << "Error: setFormat: format " << format.format << " not currently supported.  Mode=" << mode->name << ", ant=" << antName << "." << endl;
		return 0;
	}

	strcpy(D->datastream[dsId].dataSource, "MODULE");

	for(int a = 0; a < D->nAntenna; a++)
	{
		if(strcmp(D->antenna[a].name, antName.c_str()) == 0)
		{
			if(D->antenna[a].networkPort != 0)
			{
				strcpy(D->datastream[dsId].dataSource, "NETWORK");
			}
			else if(D->antenna[a].nFile > 0)
			{
				strcpy(D->datastream[dsId].dataSource, "FILE");
			}
		}
	}

	D->datastream[dsId].quantBits = format.nBit;
	DifxDatastreamAllocRecChans(D->datastream + dsId, n2);

	for(vector<VexIF>::const_iterator i = format.ifs.begin(); i != format.ifs.end(); i++)
	{
		if(i->subbandId < 0 || i->subbandId >= mode->subbands.size())
		{
			cerr << "Error: setFormat: index to subband=" << i->subbandId << " is out of range" << endl;
			exit(0);
		}
		int r = i->recordChan;
		const VexSubband& subband = mode->subbands[i->subbandId];
		int fqId = getFreqId(freqs, subband.freq, subband.bandwidth, subband.sideBand);
		
		if(r < 0 || r >= D->datastream[dsId].nRecChan)
		{
			cerr << "Error: setFormat: index to record channel = " << r << " is out of range" << endl;
			exit(0);
		}
		D->datastream[dsId].RCfreqId[r] = getBand(bandMap, fqId);
		D->datastream[dsId].RCpolName[r] = subband.pol;
	}
	DifxDatastreamAllocFreqs(D->datastream + dsId, bandMap.size());
	for(unsigned int j = 0; j < bandMap.size(); j++)
	{
		D->datastream[dsId].freqId[j] = bandMap[j].first;
		D->datastream[dsId].nPol[j] = bandMap[j].second;
	}

	return n2;
}

// FIXME -- kind of ugly to pass bandwidth on command line param.  Need a better way to 
// send bandwidth around.  This is all confused due to oversampling
static void populateFreqTable(DifxInput *D, const vector<freq>& freqs, double bw)
{
	D->nFreq = freqs.size();
	D->freq = newDifxFreqArray(D->nFreq);
	for(unsigned int f = 0; f < freqs.size(); f++)
	{
		D->freq[f].freq = freqs[f].fq/1.0e6;
		//D->freq[f].bw   = freqs[f].bw/1.0e6;
		D->freq[f].bw   = bw/1.0e6;
		D->freq[f].sideband = freqs[f].sideBand;
	}
}

// warning: assumes same number of datastreams == antennas for each config
static void populateBaselineTable(DifxInput *D, const CorrParams *P, const CorrSetup *corrSetup, int overSamp)
{	
	int a1, a2, f, n1, n2, u, v;
	int npol;
	int a1c[2], a2c[2];
	char a1p[2], a2p[2];
	int nFreq;
	DifxBaseline *bl;
	DifxConfig *config;
	int freqId, blId, configId;


	// Calculate maximum number of possible baselines based on list of configs
	D->nBaseline = 0;

	// FIXME : below assumes nAntenna = nDatastream!
	if(P->v2dMode == V2D_MODE_PROFILE)
	{
		// Here use nAntenna as nBaseline
		for(configId = 0; configId < D->nConfig; configId++)
		{
			int nD = D->config[configId].nDatastream;
			D->nBaseline += nD;
		}
	}
	else
	{
		// This is the normal configuration, assume n*(n-1)/2
		for(configId = 0; configId < D->nConfig; configId++)
		{
			int nD = D->config[configId].nDatastream;
			D->nBaseline += nD*(nD-1)/2;
		}
	}

	D->baseline = newDifxBaselineArray(D->nBaseline);

	bl = D->baseline;
	blId = 0;	// baseline table index

	for(configId = 0; configId < D->nConfig; configId++)
	{
		config = D->config + configId;

		config->nBaseline = 0;

		// Note: these should loop over antennaIds
		if(P->v2dMode == V2D_MODE_PROFILE)
		{
			// Disable writing of standard autocorrelations
			config->writeAutocorrs = 0;

			// Instead, make autocorrlations from scratch
			for(a1 = 0; a1 < config->nDatastream; a1++)
			{
				bl->dsA = config->datastreamId[a1];
				bl->dsB = config->datastreamId[a1];

				DifxBaselineAllocFreqs(bl, D->datastream[a1].nFreq);

				nFreq = 0; // this counts the actual number of freqs

				// Note: here we need to loop over all datastreams associated with this antenna!
				for(f = 0; f < D->datastream[a1].nFreq; f++)
				{
					freqId = D->datastream[a1].freqId[f];

					if(!corrSetup->correlateFreqId(freqId))
					{
						continue;
					}

					DifxBaselineAllocPolProds(bl, nFreq, 4);

					n1 = DifxDatastreamGetRecChans(D->datastream+a1, freqId, a1p, a1c);

					npol = 0;
					for(u = 0; u < n1; u++)
					{
						bl->recChanA[nFreq][npol] = a1c[u];
						bl->recChanB[nFreq][npol] = a1c[u];
						npol++;
					}

					if(npol == 0)
					{
						// This deallocates
						DifxBaselineAllocPolProds(bl, nFreq, 0);

						continue;
					}

					if(npol == 2 && corrSetup->doPolar)
					{
						// configure cross hands here
						bl->recChanA[nFreq][2] = bl->recChanA[nFreq][0];
						bl->recChanB[nFreq][2] = bl->recChanB[nFreq][1];
						bl->recChanA[nFreq][3] = bl->recChanA[nFreq][1];
						bl->recChanB[nFreq][3] = bl->recChanB[nFreq][0];
					}
					else
					{
						// Not all 4 products used: reduce count
						bl->nPolProd[nFreq] = npol;
					}

					nFreq++;
				}

				bl->nFreq = nFreq;

				if(bl->nFreq > 0)
				{
					config->baselineId[config->nBaseline] = blId;
					config->nBaseline++;
					bl++;
					blId++;
				}
			}
		}
		else
		{
			for(a1 = 0; a1 < config->nDatastream-1; a1++)
			{
				for(a2 = a1+1; a2 < config->nDatastream; a2++)
				{
					bl->dsA = config->datastreamId[a1];
					bl->dsB = config->datastreamId[a2];

					if(!P->useBaseline(D->antenna[a1].name, D->antenna[a2].name))
					{
						continue;
					}

					DifxBaselineAllocFreqs(bl, D->datastream[a1].nFreq);

					nFreq = 0; // this counts the actual number of freqs

					// Note: here we need to loop over all datastreams associated with this antenna!
					for(f = 0; f < D->datastream[a1].nFreq; f++)
					{
						freqId = D->datastream[a1].freqId[f];

						if(!corrSetup->correlateFreqId(freqId))
						{
							continue;
						}

						DifxBaselineAllocPolProds(bl, nFreq, 4);

						n1 = DifxDatastreamGetRecChans(D->datastream+a1, freqId, a1p, a1c);
						n2 = DifxDatastreamGetRecChans(D->datastream+a2, freqId, a2p, a2c);

						npol = 0;
						for(u = 0; u < n1; u++)
						{
							for(v = 0; v < n2; v++)
							{
								if(a1p[u] == a2p[v])
								{
									bl->recChanA[nFreq][npol] = a1c[u];
									bl->recChanB[nFreq][npol] = a2c[v];
									npol++;
								}
							}
						}

						if(npol == 0)
						{
							// This deallocates
							DifxBaselineAllocPolProds(bl, nFreq, 0);

							continue;
						}

						if(npol == 2 && corrSetup->doPolar)
						{
							// configure cross hands here
							bl->recChanA[nFreq][2] = bl->recChanA[nFreq][0];
							bl->recChanB[nFreq][2] = bl->recChanB[nFreq][1];
							bl->recChanA[nFreq][3] = bl->recChanA[nFreq][1];
							bl->recChanB[nFreq][3] = bl->recChanB[nFreq][0];
						}
						else
						{
							// Not all 4 products used: reduce count
							bl->nPolProd[nFreq] = npol;
						}

						nFreq++;
					}

					bl->nFreq = nFreq;

					if(bl->nFreq > 0)
					{
						config->baselineId[config->nBaseline] = blId;
						config->nBaseline++;
						bl++;
						blId++;
					}
				}
			}
		}
		config->baselineId[config->nBaseline] = -1;
	}

	// set actual number of baselines
	D->nBaseline = blId;
}

static void populateEOPTable(DifxInput *D, const vector<VexEOP>& E)
{
	int nEOP;

	nEOP = E.size();
	D->nEOP = nEOP;
	D->eop = newDifxEOPArray(D->nEOP);
	for(int e = 0; e < nEOP; e++)
	{
		D->eop[e].mjd = static_cast<int>(E[e].mjd);
		D->eop[e].tai_utc = static_cast<int>(E[e].tai_utc);
		D->eop[e].ut1_utc = E[e].ut1_utc;
		D->eop[e].xPole = E[e].xPole*180.0*3600.0/M_PI;
		D->eop[e].yPole = E[e].yPole*180.0*3600.0/M_PI;
	}
}

static int getConfigIndex(vector<pair<string,string> >& configs, DifxInput *D, const VexData *V, const CorrParams *P, const VexScan *S)
{
	int c;
	DifxConfig *config;
	const CorrSetup *corrSetup;
	const VexMode *mode;
	string configName;
	double sendLength;
	double minBW;

	corrSetup = P->getCorrSetup(S->corrSetupName);
	if(corrSetup == 0)
	{
		cerr << "Error: correlator setup[" << S->corrSetupName << "] == 0" << endl;
		exit(0);
	}

	mode = V->getMode(S->modeName);
	if(mode == 0)
	{
		cerr << "Error: mode[" << S->modeName << "] == 0" << endl;
		exit(0);
	}

	for(unsigned int i = 0; i < configs.size(); i++)
	{
		if(configs[i].first  == S->modeName &&
		   configs[i].second == S->corrSetupName)
		{
			return i;
		}
	}

	sendLength = P->sendLength;

	configName = S->modeName + string("_") + S->corrSetupName;

	c = configs.size();
	configs.push_back(pair<string,string>(S->modeName, S->corrSetupName));
	config = D->config + c;
	strcpy(config->name, configName.c_str());
	config->tInt = corrSetup->tInt;
	if(corrSetup->specAvg == 0)
	{
		config->nChan = corrSetup->nChan;
		config->specAvg = 1;	
		if(corrSetup->nChan < 128)
		{
			config->specAvg = 128/corrSetup->nChan;
			config->nChan = 128;
		}
	}
	else
	{
		config->nChan = corrSetup->nChan*corrSetup->specAvg;
		config->specAvg = corrSetup->specAvg;
	}
	config->guardBlocks = 1;
	config->postFFringe = corrSetup->postFFringe;
	config->quadDelayInterp = 1;
	config->pulsarId = -1;		// FIXME -- from setup
	config->doPolar = corrSetup->doPolar;
	config->nAntenna = D->nAntenna;
	config->nDatastream = D->nAntenna;
	config->nBaseline = D->nAntenna*(D->nAntenna-1)/2;
	if(config->overSamp <= 0)
	{
		cerr << "Error: configName=" << configName << " overSamp=" << config->overSamp << endl;
		cerr << "samprate=" << mode->sampRate << " bw=" << 
			mode->subbands[0].bandwidth << endl;
		exit(0);
	}
	minBW = mode->sampRate/(2.0*config->decimation);
	DifxConfigAllocDatastreamIds(config, config->nDatastream, c*config->nDatastream);
	DifxConfigAllocBaselineIds(config, config->nBaseline, c*config->nBaseline);

	config->nPol = mode->getPols(config->pol);
	config->quantBits = mode->getBits();
	if(corrSetup->blocksPerSend > 0)
	{
		config->blocksPerSend = corrSetup->blocksPerSend;
	}
	else
	{
		int blocksPerInt = (int)(corrSetup->tInt*minBW/config->nChan + 0.5);
		int bytesPerBlock = (int)(mode->subbands.size()*config->nChan*2*mode->getBits()/8 + 0.5);
		int bps = 1;
		int sendBytes = (int)(sendLength*mode->subbands.size()*mode->sampRate*mode->getBits()/(8.0*config->overSamp) + 0.5);

		// Search for the perfect number of blocks per send
		for(int b = 1; b <=blocksPerInt; b++)
		{
			if(blocksPerInt % b == 0)
			{
				if(b*bytesPerBlock < 2*sendBytes)
				{
					bps = b;
				}
			}
		}

		if(bps * bytesPerBlock < sendBytes/5.0)
		{
			// Give up on even number of blocks per integration
			bps = (int)(sendBytes/bytesPerBlock + 0.5);
		}

		config->blocksPerSend = bps;
	}

	// FIXME -- reset sendLength based on blockspersend, then readjust tInt, perhaps

	return c;
}

int writeJob(const VexJob& J, const VexData *V, const CorrParams *P, int overSamp, int verbose, ofstream *of, int nDigit, char ext)
{
	DifxInput *D;
	DifxScan *scan;
	string corrSetupName;
	const CorrSetup *corrSetup;
	const SourceSetup *sourceSetup;
	const VexMode *mode;
	const VexScan *S;
	set<string> configSet;
	set<string> spacecraftSet;
	vector<pair<string,string> > configs;
	vector<string> antList;
	vector<freq> freqs;
	int nPulsar=0;

	// Assume same correlator setup for all scans

	if(J.scans.size() == 0)
	{
		cerr << "Developer error: writeJob(): J.scans.size() = 0" << endl;
		exit(0);
	}
	
	S = V->getScan(J.scans.front());
	if(!S)
	{
		cerr << "Developer error: writeJob() top: scan[" << J.scans.front() << "] = 0" << endl;
		exit(0);
	}
	corrSetupName = S->corrSetupName;
	corrSetup = P->getCorrSetup(corrSetupName);
	if(!corrSetup)
	{
		cerr << "Error: writeJob(): correlator setup " << corrSetupName << "Not found!" << endl;
		exit(0);
	}

	// make set of unique config names
	for(vector<string>::const_iterator si = J.scans.begin(); si != J.scans.end(); si++)
	{
		string configName;

		S = V->getScan(*si);
		if(!S)
		{
			cerr << "Developer error: writeJob() loop: scan[" << *si << "] = 0" << endl;
			exit(0);
		}
		configName = S->modeName + string("_") + S->corrSetupName;
		configSet.insert(configName);
	}


	D = newDifxInput();

	D->mjdStart = J.mjdStart;
	D->mjdStop  = J.mjdStop;
	D->visBufferLength = P->visBufferLength;
	D->startChan = corrSetup->startChan;
	D->nOutChan = corrSetup->nOutChan;
	D->dataBufferFactor = P->dataBufferFactor;
	D->nDataSegments = P->nDataSegments;

	D->antenna = makeDifxAntennas(J, V, P, &(D->nAntenna), antList);
	D->job = makeDifxJob(V->getDirectory(), J, D->nAntenna, V->getExper()->name, &(D->nJob), nDigit, ext);
	
	D->nScan = J.scans.size();
	D->scan = newDifxScanArray(D->nScan);
	D->nConfig = configSet.size();
	D->config = newDifxConfigArray(D->nConfig);
	for(int c = 0; c < D->nConfig; c++)
	{
		D->config[c].decimation = overSamp;
		D->config[c].overSamp = overSamp;
	}
		
	// now run through all scans, populating things as we go
	scan = D->scan;
	for(vector<string>::const_iterator si = J.scans.begin(); si != J.scans.end(); si++, scan++)
	{
		S = V->getScan(*si);
		if(!S)
		{
			cerr << "Error: source[" << *si << "] not found!  This cannot be!" << endl;
			exit(0);
		}

		const VexSource *src = V->getSource(S->sourceName);

		// Determine interval where scan and job overlap
		VexInterval scanInterval(*S);
		scanInterval.logicalAnd(J);

		corrSetup = P->getCorrSetup(S->corrSetupName);
		sourceSetup = P->getSourceSetup(S->sourceName);

		scan->mjdStart = scanInterval.mjdStart;
		scan->mjdEnd = scanInterval.mjdStop;
		scan->startPoint = static_cast<int>((scanInterval.mjdStart - J.mjdStart)*86400.0/D->job->modelInc + 0.01);
		scan->nPoint = static_cast<int>(scanInterval.duration_seconds()/D->job->modelInc + 0.01);
		scan->configId = getConfigIndex(configs, D, V, P, S);

		scan->ra = src->ra;
		scan->dec = src->dec;
		if(src->calCode > ' ')
		{
			scan->calCode[0] = src->calCode;
			scan->calCode[1] = 0;
		}
		strcpy(scan->name, S->sourceName.c_str());
		// FIXME qual and calcode

		if(sourceSetup)
		{
			if(sourceSetup->ra > -990)
			{
				scan->ra = sourceSetup->ra;
			}
			if(sourceSetup->dec > -990)
			{
				scan->dec = sourceSetup->dec;
			}
			if(sourceSetup->difxName.size() > 0)
			{
				strcpy(scan->name, sourceSetup->difxName.c_str());
			}
			if(sourceSetup->ephemFile.size() > 0)
			{
				spacecraftSet.insert(S->sourceName);
			}
		}
	}

	for(int c = 0; c < D->nConfig; c++)
	{
		corrSetup = P->getCorrSetup(configs[c].second);
		if(corrSetup->binConfigFile.size() > 0)
		{
			nPulsar++;
		}
	}
	if(nPulsar > 0)
	{
		D->pulsar = newDifxPulsarArray(nPulsar);
	}

	// configure datastreams
	D->datastream = makeDifxDatastreams(J, V, D->nConfig, &(D->nDatastream));
	D->nDatastream = 0;
	for(int c = 0; c < D->nConfig; c++)
	{
		mode = V->getMode(configs[c].first);
		if(mode == 0)
		{
			cerr << "Error: mode[" << configs[c].first << "] is null" << endl;
			exit(0);
		}

		corrSetup = P->getCorrSetup(configs[c].second);
		if(corrSetup == 0)
		{
			cerr << "Error: correlator setup[" << configs[c].second << "] is null" << endl;
			exit(0);
		}

		if(corrSetup->binConfigFile.size() > 0)
		{
			D->config[c].pulsarId = D->nPulsar;
			strcpy(D->pulsar[D->nPulsar].fileName, corrSetup->binConfigFile.c_str());
			D->nPulsar++;
		}

		int d = 0;
		for(int a = 0; a < D->nAntenna; a++)
		{
			string antName = antList[a];
			int v = setFormat(D, D->nDatastream, freqs, mode, antName);
			if(v)
			{
				if(J.getVSN(antName) == "None")
				{
					strcpy(D->datastream[D->nDatastream].dataSource, "FILE");
				}
				D->config[c].datastreamId[d] = D->nDatastream;
				D->nDatastream++;
				d++;
			}
		}
	}

	if(nPulsar != D->nPulsar)
	{
		cerr << "Error: nPulsar=" << nPulsar << " != D->nPulsar=" << D->nPulsar << endl;
		exit(0);
	}

	// Populate spacecraft table (must be before deriveSourceTable()
	if(!spacecraftSet.empty())
	{
		DifxSpacecraft *ds;
		double fracday0, deltat;
		int mjdint, n0, nPoint, v;
		double mjd0;

		D->spacecraft = newDifxSpacecraftArray(spacecraftSet.size());
		D->nSpacecraft = spacecraftSet.size();
		
		ds = D->spacecraft;

		for(set<string>::const_iterator s = spacecraftSet.begin(); s != spacecraftSet.end(); s++, ds++)
		{
			sourceSetup = P->getSourceSetup(*s);

			mjdint = static_cast<int>(J.mjdStart);
			fracday0 = J.mjdStart-mjdint;
			deltat = sourceSetup->ephemDeltaT/86400.0;	// convert from seconds to days
			n0 = static_cast<int>(fracday0/deltat - 2);	// start ephmemeris at least 2 points early
			mjd0 = mjdint + (n0-5)*deltat;			// always start an integer number of increments into day
			nPoint = static_cast<int>(J.duration()/deltat) + 11; // make sure to extend beyond the end of the job
			if(verbose > 0)
			{
				cout << "Computing ephemeris:" << endl;
				cout << "  vex source name = " << sourceSetup->difxName << endl;
				cout << "  object name = " << sourceSetup->ephemObject << endl;
				cout << "  mjd = " << mjdint << "  deltat = " << deltat << endl;
				cout << "  startPoint = " << n0 << "  nPoint = " << nPoint << endl;
				cout << "  ephemFile = " << sourceSetup->ephemFile << endl;
				cout << "  naifFile = " << sourceSetup->naifFile << endl;
			}
			v = computeDifxSpacecraftEphemeris(ds, mjd0, deltat, nPoint, 
				sourceSetup->ephemObject.c_str(),
				sourceSetup->naifFile.c_str(),
				sourceSetup->ephemFile.c_str());
			if(v != 0)
			{
				cerr << "Error: ephemeris calculation failed.  Must stop." << endl;
				exit(0);
			}

			// give the spacecraft table the right name so it can be linked to the source
			if(sourceSetup->difxName.size() > 0)
			{
				strcpy(ds->name, sourceSetup->difxName.c_str());
			}
			else
			{
				strcpy(ds->name, sourceSetup->vexName.c_str());
			}
		}
	}

	// Make frequency table
	mode = V->getMode(configs[0].first);
	double bw = mode->sampRate/(2.0*overSamp);
	populateFreqTable(D, freqs, bw);

	// Make baseline table
	populateBaselineTable(D, P, corrSetup, overSamp);

	// Make EOP table
	populateEOPTable(D, V->getEOPs());

	// complete a few DifxInput structures
	deriveSourceTable(D);

	// Merge identical table entries
	simplifyDifxFreqs(D);
	simplifyDifxDatastreams(D);
	simplifyDifxBaselines(D);
	simplifyDifxConfigs(D);

	if(P->simFXCORR)
	{
		// nudge integration times and start times to match those of the VLBA HW correlator
		DifxInputSimFXCORR(D);
	}

	if(P->padScans)
	{
		// insert pad scans where needed
		padDifxScans(D);
	}
	else
	{
		cerr << "Warning: scans are not padded.  This may mean the correlator jobs that are produced will not work.  Set padScans to true to force the padding of scans." << endl;
	}

	// fix a few last parameters
	if(corrSetup->specAvg == 0)
	{
		D->specAvg  = D->config[0].specAvg;
	}
	else
	{
		D->specAvg = corrSetup->specAvg;
	}

	if(D->nBaseline > 0)
	{
		// write input file
		ostringstream inputName;
		inputName << D->job->fileBase << ".input";
		writeDifxInput(D, inputName.str().c_str());

		// write calc file
		ostringstream calcName;
		calcName << D->job->fileBase << ".calc";
		writeDifxCalc(D, calcName.str().c_str());

		// write flag file
		ostringstream flagName;
		flagName << D->job->fileBase << ".flag";
		J.generateFlagFile(*V, flagName.str(), P->invalidMask);

		if(verbose > 2)
		{
			printDifxInput(D);
		}

		if(of)
		{
			const char *fileBase = D->job->fileBase;
			double tops;	// Trillion operations
			int p;

			for(int i = 0; D->job->fileBase[i]; i++)
			{
				if(D->job->fileBase[i] == '/')
				{
					fileBase = D->job->fileBase + i + 1;
				}
			}

			tops = J.calcOps(V, corrSetup->nChan*2, corrSetup->doPolar) * 1.0e-12;

			*of << fileBase << " " << J.mjdStart << " " << J.mjdStop << " " << D->nAntenna << " ";
			p = of->precision();
			of->precision(4);
			*of << tops << " ";
			*of << (J.dataSize/1000000) << "  #";
			of->precision(p);
			

			for(vector<string>::const_iterator ai = antList.begin(); ai != antList.end(); ai++)
			{
				*of << " " << *ai;
			}
			*of << endl;
		}
	}
	else
	{
		cerr << "Warning: job " << D->job->fileBase << " not written since it correlates no data" << endl;
		cerr << "This is usually due to all frequency Ids being unselected." << endl;
	}

	// clean up
	deleteDifxInput(D);

	if(D->nBaseline > 0)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

static int sanityCheckSources(const VexData *V, const CorrParams *P)
{
	vector<SourceSetup>::const_iterator s;
	int nWarn = 0;

	for(s = P->sourceSetups.begin(); s != P->sourceSetups.end(); s++)
	{
		if(V->getSource(s->vexName) == 0)
		{
			cerr << "Warning: source " << s->vexName << " referenced in .v2d file but not is not in vex file" << endl;
			nWarn++;
		}
	}

	return nWarn;
}

int usage(int argc, char **argv)
{
	cout << endl;
	cout << program << " version " << version << "  " << author << " " << verdate << endl;
	cout << endl;
	cout << "Usage:  " << argv[0] << " [<options>] <v2d file>" << endl;
	cout << endl;
	cout << "  options can include:" << endl;
	cout << "     -h" << endl;
	cout << "     --help        display this information and quit." << endl;
	cout << endl;
	cout << "     -v" << endl;
	cout << "     --verbose     increase the verbosity of the output; -v -v for more." << endl;
	cout << endl;
	cout << "     -o" << endl;
	cout << "     --output      create a v2d file with all defaults populated." << endl;
	cout << endl;
	cout << "     -d" << endl;
	cout << "     --delete-old  delete all jobs in this series before running." << endl;
	cout << endl;
	cout << "     -s" << endl;
	cout << "     --strict      treat some warnings as errors and quit." << endl;
	cout << endl;
	cout << "  the v2d file is the vex2difx configuration file to process." << endl;
	cout << endl;
	cout << "See http://cira.ivec.org/dokuwiki/doku.php/difx/vex2difx for more information" << endl;
	cout << endl;

	return 0;
}

int main(int argc, char **argv)
{
	VexData *V;
	CorrParams *P;
	vector<VexJob> J;
	ifstream is;
	string shelfFile;
	int verbose = 0;
	string v2dFile;
	bool writeParams = 0;
	bool deleteOld = 0;
	bool strict = 0;
	int nWarn = 0;
	int nDigit;
	int nJob = 0;
	int nMulti = 0;

	if(argc < 2)
	{
		return usage(argc, argv);
	}

	// force program to work in Univeral Time
	setenv("TZ", "", 1);    
	tzset();


	for(int a = 1; a < argc; a++)
	{
		if(argv[a][0] == '-')
		{
			if(strcmp(argv[a], "-h") == 0 ||
			   strcmp(argv[a], "--help") == 0)
			{
				return usage(argc, argv);
			}
			else if(strcmp(argv[a], "-v") == 0 ||
			        strcmp(argv[a], "--verbose") == 0)
			{
				verbose++;
			}
			else if(strcmp(argv[a], "-o") == 0 ||
			        strcmp(argv[a], "--output") == 0)
			{
				writeParams = 1;
			}
			else if(strcmp(argv[a], "-d") == 0 ||
			        strcmp(argv[a], "--delete-old") == 0)
			{
				deleteOld = 1;
			}
			else if(strcmp(argv[a], "-s") == 0 ||
			        strcmp(argv[a], "--strict") == 0)
			{
				strict = 1;
			}
			else
			{
				cerr << "Error: unknown option " << argv[a] << endl;
				cerr << "Run with -h for help information." << endl;
				exit(0);
			}
		}
		else
		{
			if(v2dFile.size() > 0)
			{
				cerr << "Error: multiple configuration files provided, only one expected." << endl;
				cerr << "Run with -h for help information." << endl;
				exit(0);
			}
			v2dFile = argv[a];
		}
	}

	if(v2dFile.size() == 0)
	{
		cerr << "Error: configuration file (.v2d) expected." << endl;
		cerr << "Run with -h for help information." << endl;
		exit(0);
	}

	if(v2dFile.find("_") != string::npos)
	{
		cerr << "Error: you cannot have an underscore (_) in the filename!" << endl;
		cerr << "Please rename it and run again." << endl;
		exit(0);
	}

	P = new CorrParams(v2dFile);
	if(P->vexFile.size() == 0)
	{
		cerr << "Error: vex file parameter (vex) not found in file." << endl;
		exit(0);
	}

	nWarn = P->parseWarnings;

	umask(02);

	shelfFile = P->vexFile.substr(0, P->vexFile.find_last_of('.'));
	shelfFile += string(".shelf");
	nWarn += P->loadShelves(shelfFile);

	V = loadVexFile(*P);

	if(!V)
	{
		cerr << "Error: cannot load vex file: " << P->vexFile << endl;
		exit(0);
	}

	nWarn += P->sanityCheck();
	nWarn += V->sanityCheck();
	nWarn += sanityCheckSources(V, P);
	if(strict && nWarn > 0)
	{
		cerr << "Quitting since " << nWarn << 
			" warnings were found and strict mode was enabled." << endl;
		exit(0);
	}

	makeJobs(J, V, P, verbose);

	if(verbose > 1)
	{
		cout << *V << endl;
		cout << *P << endl;
	}

	if(writeParams)
	{
		ofstream of;
		string paramsFile = v2dFile + ".params";

		of.open(paramsFile.c_str());
		of << *P << endl;
		of.close();
	}

	if(deleteOld)
	{
		char cmd[512];

		sprintf(cmd, "rm -f %s.params", v2dFile.c_str());
		if(verbose > 0)
		{
			printf("Executing: %s\n", cmd);
		}
		system(cmd);

		sprintf(cmd, "rm -f %s_*.input", P->jobSeries.c_str());
		if(verbose > 0)
		{
			printf("Executing: %s\n", cmd);
		}
		system(cmd);

		sprintf(cmd, "rm -f %s_*.calc", P->jobSeries.c_str());
		if(verbose > 0)
		{
			printf("Executing: %s\n", cmd);
		}
		system(cmd);
		
		sprintf(cmd, "rm -f %s_*.flag", P->jobSeries.c_str());
		if(verbose > 0)
		{
			printf("Executing: %s\n", cmd);
		}
		system(cmd);
	}

	ofstream of;
	string jobListFile = P->jobSeries + ".joblist";
	string difxVersion(getenv("DIFX_VERSION"));
	if(difxVersion == "")
	{
		difxVersion = "unknown";
	}
	of.open(jobListFile.c_str());
	of.precision(12);
	of << "exper=" << V->getExper()->name << "  v2d=" << v2dFile <<"  pass=" << P->jobSeries << "  mjd=" << current_mjd() << "  DiFX=" << difxVersion << "  vex2difx=" << version << endl;
	
	nDigit=0;
	for(int l = J.size(); l > 0; l /= 10)
	{
		nDigit++;
	}
	
	for(vector<VexJob>::iterator j = J.begin(); j != J.end(); j++)
	{
		if(verbose > 0)
		{
			cout << *j;
		}
		const VexScan *S = V->getScan(j->scans[0]);
		const VexMode *M = V->getMode(S->modeName);
		int n = 0;
		for(list<int>::const_iterator k = M->overSamp.begin(); k != M->overSamp.end(); k++)
		{
			char ext=0;
			if(M->overSamp.size() > 1)
			{
				ext='a'+n;
			}
			nJob += writeJob(*j, V, P, *k, verbose, &of, nDigit, ext);
			n++;
		}
		if(M->overSamp.size() > 1)
		{
			nMulti++;
		}
	}
	of.close();

	cout << endl;
	cout << nJob << " job(s) created." << endl;

	if(nMulti > 0)
	{
		cout << endl;
		cout << "Notice!  " << nMulti << " jobs were replicated multiple times and have a letter suffix" << endl;
		cout << "after the job number.  This is probably due to mixed amounts of oversampling" << endl;
		cout << "at the same time within one or more observing modes.  In cases like this the" << endl;
		cout << "PI might want different processing to be done on each IF (such as number of" << endl;
		cout << "spectral lines or integration times).  Consider explicitly making multiple" << endl;
		cout << ".v2d files, one for each oversample factor, that operate only on the" << endl;
		cout << "relavant baseband channels." << endl;
	}

	delete V;
	delete P;

	cout << endl;

	return 0;
}
