//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2013 EMC Corp.
//
//	@filename:
//		CEnumeratorConfig.cpp
//
//	@doc:
//		Implementation of plan enumerator config
//
//	@owner:
//		
//
//	@test:
//
//
//---------------------------------------------------------------------------

#include "gpos/base.h"
#include "gpos/io/COstreamFile.h"
#include "gpos/memory/CAutoMemoryPool.h"
#include "gpos/task/CAutoSuspendAbort.h"
#include "gpos/error/CAutoTrace.h"
#include "gpos/task/CTask.h"
#include "gpos/task/CWorker.h"

#include "gpopt/base/CIOUtils.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/engine/CEnumeratorConfig.h"


using namespace gpos;
using namespace gpopt;


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::CEnumeratorConfig
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CEnumeratorConfig::CEnumeratorConfig
	(
	IMemoryPool *pmp,
	ULLONG ullPlanId,
	ULLONG ullSamples,
	CDouble dCostThreshold
	)
	:
	m_pmp(pmp),
	m_ullPlanId(ullPlanId),
	m_ullSpaceSize(0),
	m_ullInputSamples(ullSamples),
	m_costBest(GPOPT_INVALID_COST),
	m_costMax(GPOPT_INVALID_COST),
	m_dCostThreshold(dCostThreshold),
	m_pdrgpsp(NULL),
	m_dStep(0.5),
	m_pdX(NULL),
	m_pdY(NULL),
	m_ulDistrSize(0),
	m_fSampleValidPlans(true),
	m_pfpc(NULL)
{
	m_pdrgpsp = GPOS_NEW(pmp) DrgPsp(pmp);
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::~CEnumeratorConfig
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CEnumeratorConfig::~CEnumeratorConfig()
{
	GPOS_DELETE_ARRAY(m_pdX);
	GPOS_DELETE_ARRAY(m_pdY);
	m_pdrgpsp->Release();
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::DCostDistrX
//
//	@doc:
//		Return x-value of a fitted cost distribution
//
//---------------------------------------------------------------------------
CDouble
CEnumeratorConfig::DCostDistrX
	(
	ULONG ulPos
	)
	const
{
	GPOS_ASSERT(NULL != m_pdX);

	return m_pdX[ulPos];
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::DCostDistrY
//
//	@doc:
//		Return y-value of a fitted cost distribution
//
//---------------------------------------------------------------------------
CDouble
CEnumeratorConfig::DCostDistrY
	(
	ULONG ulPos
	)
	const
{
	GPOS_ASSERT(NULL != m_pdY);

	return m_pdY[ulPos];
}



//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::ClearSamples
//
//	@doc:
//		Clear samples
//
//---------------------------------------------------------------------------
void
CEnumeratorConfig::ClearSamples()
{
	GPOS_DELETE_ARRAY(m_pdX);
	GPOS_DELETE_ARRAY(m_pdY);
	m_ulDistrSize = 0;
	m_pdrgpsp->Clear();
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::FAddSample
//
//	@doc:
//		Add a new plan to sample
//
//---------------------------------------------------------------------------
BOOL
CEnumeratorConfig::FAddSample
	(
	ULLONG ullPlanId,
	CCost cost
	)
{
	GPOS_ASSERT(m_costBest != GPOPT_INVALID_COST);

	BOOL fAccept = (GPOPT_UNBOUNDED_COST_THRESHOLD == m_dCostThreshold) ||
					(cost <= m_costBest * m_dCostThreshold);
	if (fAccept)
	{
		m_pdrgpsp->Append(GPOS_NEW(m_pmp) SSamplePlan(ullPlanId, cost));

		if (GPOPT_INVALID_COST == m_costMax || cost > m_costMax)
		{
			m_costMax = cost;
		}
	}

	return fAccept;
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::DGaussian
//
//	@doc:
//		Compute Gaussian probability value
//
//---------------------------------------------------------------------------
DOUBLE
CEnumeratorConfig::DGaussian
	(
	DOUBLE d,
	DOUBLE dMean,
	DOUBLE dStd
	)
{
	const DOUBLE dE = 2.71828182846; // e: natural logarithm base
	const DOUBLE dSqrt2pi = 2.50662827463; // sqrt(2*pi)
	DOUBLE diff = clib::DPow( (d - dMean) / dStd, 2.0);

	// compute Gaussian probability:
	// G(x) = \frac{1}{\sigma * \sqrt{2 \pi}} e^{-0.5 * (\frac{x-\mu}{\sigma})^2}
	return (1.0 / (dStd * dSqrt2pi)) * clib::DPow(dE, -0.5 * diff);
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::InitCostDistrSize
//
//	@doc:
//		Initialize size of cost distribution
//
//---------------------------------------------------------------------------
void
CEnumeratorConfig::InitCostDistrSize()
{
	//  bound estimated distribution using relative cost of most expensive plan
	DOUBLE dMax = clib::DLog2(CDouble((m_costMax / CostBest())).DVal());

	// fix number of points in estimated distribution to 100
	m_dStep = CDouble(dMax / 100.0);

	// compute target distribution size
	m_ulDistrSize = (ULONG) (clib::DFloor(dMax / m_dStep.DVal()) + 1.0);
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::GussianKernelDensity
//
//	@doc:
//		Compute Gaussian Kernel density
//
//---------------------------------------------------------------------------
void
CEnumeratorConfig::GussianKernelDensity
	(
	DOUBLE *pdObervationX,
	DOUBLE *pdObervationY,
	ULONG ulObservations,
	DOUBLE *pdX, // input: X-values we need to compute estimates for
	DOUBLE *pdY, // output: estimated Y-values for given X-values
	ULONG ulSize // number of input X-values
	)
{
	GPOS_ASSERT(NULL != pdObervationX);
	GPOS_ASSERT(NULL != pdObervationY);
	GPOS_ASSERT(NULL != pdX);
	GPOS_ASSERT(NULL != pdY);
	GPOS_ASSERT(pdX != pdY);

	// finding observations span to determine kernel bandwidth
	DOUBLE dMin = pdObervationX[0];
	DOUBLE dMax = pdObervationX[0];
	for (ULONG ul = 1; ul < ulObservations; ul++)
	{
		if (pdObervationX[ul] > dMax)
		{
			dMax = pdObervationX[ul];
		}

		if (pdObervationX[ul] < dMin)
		{
			dMin = pdObervationX[ul];
		}
	}
	GPOS_ASSERT(dMax >= dMin);

	// kernel bandwidth set to 1% of distribution span
	DOUBLE dBandWidth = 0.01 * (dMax - dMin);
	for (ULONG ul = 0; ul < ulSize; ul++)
	{
		DOUBLE dx = pdX[ul];
		DOUBLE dy = 0;
		for (ULONG ulObs = 0; ulObs < ulObservations; ulObs++)
		{
			DOUBLE dObsX = pdObervationX[ulObs];
			DOUBLE dObsY = pdObervationY[ulObs];
			DOUBLE dDiff = (dx - dObsX) / dBandWidth;
			dy = dy +  dObsY * DGaussian(dDiff, 0.0, 1.0);
		}
		dy = dy / (dBandWidth * ulObservations);

		pdY[ul] = dy;
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::FitCostDistribution
//
//	@doc:
//		Fit cost distribution on generated samples
//
//---------------------------------------------------------------------------
void
CEnumeratorConfig::FitCostDistribution()
{
	GPOS_DELETE_ARRAY(m_pdX);
	GPOS_DELETE_ARRAY(m_pdY);
	InitCostDistrSize();
	ULONG ulCreatedSamples = UlCreatedSamples();
	m_pdX = GPOS_NEW_ARRAY(m_pmp, DOUBLE, m_ulDistrSize);
	m_pdY = GPOS_NEW_ARRAY(m_pmp, DOUBLE, m_ulDistrSize);
	DOUBLE *pdObervationX = GPOS_NEW_ARRAY(m_pmp, DOUBLE, ulCreatedSamples);
	DOUBLE *pdObervationY = GPOS_NEW_ARRAY(m_pmp, DOUBLE, ulCreatedSamples);

	for (ULONG ul = 0; ul < ulCreatedSamples; ul++)
	{
		pdObervationX[ul] = clib::DLog2(CDouble((CostPlanSample(ul) / CostBest())).DVal());
		pdObervationY[ul] = 1.0;
	}

	DOUBLE d = 0.0;
	for (ULONG  ul = 0; ul < m_ulDistrSize; ul++)
	{
		m_pdX[ul] = d;
		m_pdY[ul] = 0.0;
		d = d + m_dStep.DVal();
	}

	GussianKernelDensity(pdObervationX, pdObervationY, ulCreatedSamples, m_pdX, m_pdY, m_ulDistrSize);

	GPOS_DELETE_ARRAY(pdObervationX);
	GPOS_DELETE_ARRAY(pdObervationY);
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::DumpSamples
//
//	@doc:
//		Dump generated samples to an output file
//
//---------------------------------------------------------------------------
void
CEnumeratorConfig::DumpSamples
	(
	CWStringDynamic *pstr, // samples dump
	ULONG ulSessionId,
	ULONG ulCommandId
	)
{
	GPOS_ASSERT(NULL != pstr);

	CAutoSuspendAbort asa;

	// dump samples to output file
	CHAR szFileName[GPOS_FILE_NAME_BUF_SIZE];
	CUtils::GenerateFileName(szFileName, "SamplePlans", "xml", GPOS_FILE_NAME_BUF_SIZE, ulSessionId, ulCommandId);
	CHAR *sz = CUtils::SzFromWsz(m_pmp, const_cast<WCHAR *>(pstr->Wsz()));
	CIOUtils::Dump(szFileName, sz);
	GPOS_DELETE_ARRAY(sz);

	// reset time slice
#ifdef GPOS_DEBUG
	CWorker::PwrkrSelf()->ResetTimeSlice();
#endif // GPOS_DEBUG
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::DumpCostDistr
//
//	@doc:
//		Dump fitted cost distribution to an output file
//
//---------------------------------------------------------------------------
void
CEnumeratorConfig::DumpCostDistr
	(
	CWStringDynamic *pstr, // cost distribution dump
	ULONG ulSessionId,
	ULONG ulCommandId
	)
{
	GPOS_ASSERT(NULL != pstr);

	CAutoSuspendAbort asa;

	// dump cost distribution to output file
	CHAR szFileName[GPOS_FILE_NAME_BUF_SIZE];
	CUtils::GenerateFileName(szFileName, "CostDistr", "xml", GPOS_FILE_NAME_BUF_SIZE, ulSessionId, ulCommandId);
	CHAR *sz = CUtils::SzFromWsz(m_pmp, const_cast<WCHAR *>(pstr->Wsz()));
	CIOUtils::Dump(szFileName, sz);
	GPOS_DELETE_ARRAY(sz);

	// reset time slice
#ifdef GPOS_DEBUG
	CWorker::PwrkrSelf()->ResetTimeSlice();
#endif // GPOS_DEBUG
}


//---------------------------------------------------------------------------
//	@function:
//		CEnumeratorConfig::PrintPlanSample
//
//	@doc:
//		Print ids of plans in the generated sample
//
//---------------------------------------------------------------------------
void
CEnumeratorConfig::PrintPlanSample() const
{
	CAutoTrace at(m_pmp);

	const ULONG ulSamples = UlCreatedSamples();
	at.Os() << "[OPT]: Generated "<< ulSamples <<" plan samples: ";
	if (0 < ulSamples)
	{
		// print a message with the ids of generated samples

		for (ULONG ul = 0; ul < ulSamples - 1; ul++)
		{
			at.Os() << UllPlanSample(ul) + 1 << ", ";
		}
		at.Os() << UllPlanSample(ulSamples - 1) + 1 << std::endl;
	}
}

// EOF