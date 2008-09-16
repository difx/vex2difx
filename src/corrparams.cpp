#include "corrparams.h"

bool CorrParams::useAntenna(const string &antName)
{
	int i, n;

	n = antennaList.size();
	if(n == 0)
	{
		return true;
	}
	for(i = 0; i < n; i++)
	{
		if(antName == antennaList[i])
		{
			return true;
		}
		// FIXME -- allow -antName
	}
}
