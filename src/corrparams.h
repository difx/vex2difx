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

#ifndef __CORRPARAM_H__
#define __CORRPARAM_H__

#include <string>
#include <set>
#include <vector>
#include <map>
#include <list>
#include <difxio.h>

#include "vextables.h"

using namespace std;

extern const double MJD_UNIX0;	// MJD at beginning of unix time
extern const double SEC_DAY;
extern const double MUSEC_DAY;

enum V2D_Mode
{
	V2D_MODE_NORMAL = 0,	// for almost all purposes
	V2D_MODE_PROFILE = 1	// to produce pulsar profiles
};

// see http://cira.ivec.org/dokuwiki/doku.php/difx/configuration

class PhaseCentre
{
public:
	//constants
	static const string DEFAULT_NAME;
	static const double DEFAULT_RA  = -999.9;
	static const double DEFAULT_DEC = -999.9;

	//constructors
	PhaseCentre(double ra, double dec, string name);
	PhaseCentre();

	//methods
	void initialise(double ra, double dec, string name);

	//variables
	double ra;	  //radians
	double dec;	  //radians
	string difxName;
        char calCode;
	int qualifier;
	// ephemeris
	string ephemObject;     // name of the object in the ephemeris
	string ephemFile;       // file containing a JPL ephemeris
	string naifFile;        // file containing naif time data
	double ephemDeltaT;     // tabulated ephem. interval (seconds, default 60)
};

class SourceSetup
{
public:
	SourceSetup(const string &name);
	int setkv(const string &key, const string &value);
	int setkv(const string &key, const string &value, PhaseCentre * pc);

	bool doPointingCentre;       	  // Whether or not to correlate the pointing centre
	string vexName;		     	  // Source name as appears in vex file
	PhaseCentre pointingCentre;  	  // The source which is at the pointing centre
	vector<PhaseCentre> phaseCentres; // Additional phase centres to be correlated
};

class ZoomFreq
{
public:
	//constructor
	ZoomFreq();

	//method
	void initialise(double freq, double bw, bool corrparent, int specavg);

	//variables
	double frequency, bandwidth;
	int spectralaverage;
	bool correlateparent;
};

class AntennaSetup
{
public:
	AntennaSetup(const string &name);
	int setkv(const string &key, const string &value);
	int setkv(const string &key, const string &value, ZoomFreq * zoomFreq);

	string vexName;		// Antenna name as it appears in vex file
	string difxName;	// Antenna name (if different) to appear in difx
	double X, Y, Z;		// Station coordinates to override vex
	int clockorder;		// Order of clock poly (if overriding)
	double clock2, clock3, clock4, clock5;	// Clock coefficients (if overriding)
        vector<double> freqClockOffs; // clock offsets for the individual frequencies
	VexClock clock;
	double deltaClock;	// sec
	double deltaClockRate;	// sec/sec
	// flag
	// media
	bool polSwap;		// If true, swap polarizations
	string format;		// Override format from .v2d file.  
				// This is sometimes needed because format not known always at scheduling time
				// Possible values: S2 VLBA MkIV/Mark4 Mark5B . Is converted to all caps on load
	enum DataSource dataSource;
	vector<VexBasebandFile> basebandFiles;	// files to correlate
	int networkPort;	// For eVLBI : port for this antenna
	int windowSize;		// For eVLBI : TCP window size
	int phaseCalIntervalMHz;// 0 if no phase cal extraction, positive gives interval between tones to extract
	vector<ZoomFreq> zoomFreqs;//List of zoom freqs to add for this antenna
};

class CorrSetup
{
public:
	CorrSetup(const string &name = "setup_default");
	int setkv(const string &key, const string &value);
	bool correlateFreqId(int freqId) const;
	double bytesPerSecPerBLPerBand() const;
	int checkValidity();

	string corrSetupName;

	double tInt;		// integration time
	bool doPolar;		// false for no cross pol, true for full pol
	bool doAuto;		// write autocorrelations
	int subintNS;		// Duration of a subintegration in nanoseconds
	int guardNS;		// Number of "guard" ns tacked on to end of a send
	int nChan;		// For the narrowest band 
				// (all others will have same spectral resolution)
	int specAvg;
	int maxNSBetweenUVShifts; //Mostly for multi-phase centres
	int maxNSBetweenACAvg;	// Mostly for sending STA dumps
	int fringeRotOrder;	// 0, 1, or 2
	int strideLength;	// The number of channels to do at a time
				// when fringeRotOrder > 0
	int xmacLength;		// Number of channels to do at a time when xmac'ing
	int numBufferedFFTs;	// Number of FFTs to do in Mode before XMAC'ing
	set<int> freqIds;       // which bands to correlate
	string binConfigFile;
	string phasedArrayConfigFile;
private:
	void addFreqId(int freqId);
};


class CorrRule
{
public:
	CorrRule(const string &name = "rule_default");

	int setkv(const string &key, const string &value);
	bool match(const string &scan, const string &source, const string &mode, char cal, int qual) const;

	string ruleName;

	list<string> scanName;
	list<string> sourceName;
	list<string> modeName;
	list<char> calCode;
	list<int> qualifier;

	string corrSetupName;	/* pointer to CorrSetup */
};

class CorrParams : public VexInterval
{
public:
	CorrParams();
	CorrParams(const string &fileName);

	int loadShelves(const string &fileName);
	const char *getShelf(const string &vsn) const;

	int setkv(const string &key, const string &value);
	int load(const string &fileName);
	void defaults();
	void defaultSetup();
	void defaultRule();
	void example();
	int sanityCheck();
	void addSourceSetup(SourceSetup toadd);

	bool useAntenna(const string &antName) const;
	bool useBaseline(const string &ant1, const string &ant2) const;
	bool swapPol(const string &antName) const;
	const CorrSetup *getCorrSetup(const string &name) const;
	const SourceSetup *getSourceSetup(const string &name) const;
	const SourceSetup *getSourceSetup(const vector<string> &names) const;
	const PhaseCentre *getPhaseCentre(const string &difxname) const;
	const AntennaSetup *getAntennaSetup(const string &name) const;
	const VexClock *getAntennaClock(const string &antName) const;

	const string &findSetup(const string &scan, const string &source, const string &mode, char cal, int qual) const;
	
	/* global parameters */
	int parseWarnings;
	string vexFile;
	string threadsFile;
	unsigned int minSubarraySize;
	double maxGap;		// days
	bool singleScan;
	bool singleSetup;
	bool allowOverlap;
	bool mediaSplit;	// split jobs on media change
	bool padScans;
	bool simFXCORR;		// set integration and start times to match VLBA HW correlator
	bool tweakIntegrationTime;      // nadger the integration time to make values nice
	int nCore;
	int nThread;
	double maxLength;	// [days]
	double minLength;	// [days]
	double maxSize;		// [bytes] -- break jobs for output filesize
	string jobSeries;	// prefix name to job files
	int startSeries;	// start job series at this number
	int dataBufferFactor;
	int nDataSegments;
	double sendLength;	// (s) amount of data to send from datastream to core at a time
	int sendSize;           // (Bytes) amount of data to send from datastream to core at a time (overrides sendLength)
	unsigned int invalidMask;
	int visBufferLength;
	int overSamp;		// A user supplied override to oversample factor
	enum OutputFormatType outputFormat; // DIFX or ASCII

	list<string> antennaList;
	list<pair<string,string> > baselineList;

	/* manual forced job breaks */
	vector<double> manualBreaks;

	/* setups to apply */
	vector<CorrSetup> corrSetups;

	/* source setups to apply */
	vector<SourceSetup> sourceSetups;

	/* manually provided EOPs */
	vector<VexEOP> eops;

	/* antenna setups to apply */
	vector<AntennaSetup> antennaSetups;

	/* rules to determine which setups to apply */
	vector<CorrRule> rules;

	enum V2D_Mode v2dMode;

private:
	void addAntenna(const string &antName);
	void addBaseline(const string &baselineName);
	map<string,string> shelves;
};

ostream& operator << (ostream &os, const CorrSetup &x);
ostream& operator << (ostream &os, const CorrRule &x);
ostream& operator << (ostream &os, const CorrParams &x);

bool areCorrSetupsCompatible(const CorrSetup *A, const CorrSetup *B, const CorrParams *C);

#endif
