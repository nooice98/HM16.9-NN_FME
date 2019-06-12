/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2016, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file     TEncSearch.cpp
 \brief    encoder search class
 */

// Eigen Library has to be included first, or I'll get errors using namespace Eigen
#include <eigen3/Eigen/Dense>
using namespace Eigen;
#include <cnl/fixed_point.h>
using cnl::fixed_point;

#include "TLibCommon/CommonDef.h"
#include "TLibCommon/TComRom.h"
#include "TLibCommon/TComMotionInfo.h"
#include "TEncSearch.h"
#include "TLibCommon/TComTU.h"
#include "TLibCommon/Debug.h"
#include <math.h>
#include <limits>
#include <fstream>
#include <iostream>

// EMI: Parameters declaration

signed short MVX_HALF, MVX_QRTER, MVY_HALF, MVY_QRTER = 0;
std::vector<uint> array_e;
uint PUHeight, PUWidth, C;

// Used to store index of Maximum element of Output layer
MatrixXf::Index NN_out;

/*
The next set of variables are part of Eigen library for Matrix and Array manipulations
The "Array" object arithmetic performs element-wise operations, hence will be used for adding Bias and BatchNorm
The "Matrix" object arithmetic performs matrix operations, hence it's used mainly for weight multiplications
We can transform to Array or to Matrix using .array() and .matrix()
*/
Array<fixed_point<int16_t, -11>, 22, 1> X1; Array<fixed_point<int16_t, -11>, 20, 1> X2; Array<fixed_point<int16_t, -11>, 49, 1> OUT;
Array<fixed_point<int16_t, -11>, 4, 1> IN_embs0, IN_embs1; Array<fixed_point<int16_t, -11>, 17, 1> IN;
Array<fixed_point<int16_t, -11>, 8, 4> embs0, embs1;
Matrix<fixed_point<int16_t, -11>, 22, 17> in_h1;
Matrix<fixed_point<int16_t, -11>, 20, 22> h1_h2;
Matrix<fixed_point<int16_t, -11>, 49, 20> h2_out;
Array<fixed_point<int16_t, -11>, 22, 1> b1, BN_gamma_1, BN_beta_1;
Array<fixed_point<int16_t, -11>, 20, 1> b2, BN_gamma_2, BN_beta_2;
Array<fixed_point<int16_t, -11>, 49, 1> bout;
Array<fixed_point<int16_t, -11>, 9, 1> fixed_IN_errors, BN_gamma_in;
Array<float, 9, 1> IN_errors, mean, stdev;

fixed_point<int16_t, -11> relu(fixed_point<int16_t, -11> x){
	if (x>0)	{	return x; }
	else { return 0; }
}

/* ReLU function
ReLU is achieved in Eigen by using the following code:
(X1.array() < 0).select(0, X1)
This code snippet replaces all negative numbers with zeros
*/

void NN_pred(){
  
  // Normalize input values using the computed mean and standard deviations
  IN_errors << array_e[0], array_e[1], array_e[2], array_e[3], C, array_e[4], array_e[5], array_e[6], array_e[7];
  IN_errors = (IN_errors - mean) / stdev;
  fixed_IN_errors << IN_errors[0], IN_errors[1], IN_errors[2], IN_errors[3], IN_errors[4], IN_errors[5], IN_errors[6], IN_errors[7], IN_errors[8];
  // cout << fixed_IN_errors << endl << endl;
  
  // Input layer also consists of categorical variables, in which we will use embedding matrices depending on block Height and Width

  switch (PUHeight) {
    case 4:   IN_embs0 << embs0.row(1).transpose(); break;
    case 8:   IN_embs0 << embs0.row(2).transpose(); break;
    case 16:  IN_embs0 << embs0.row(3).transpose();	break;
    case 12:  IN_embs0 << embs0.row(4).transpose();	break;
    case 24:  IN_embs0 << embs0.row(5).transpose();	break;
    case 32:  IN_embs0 << embs0.row(6).transpose();	break;
    case 64:  IN_embs0 << embs0.row(7).transpose();	break;
    default:  IN_embs0 << embs0.row(0).transpose();	break;
  }

  switch (PUWidth) {
    case 4:   IN_embs1 << embs1.row(1).transpose(); break;
    case 8:   IN_embs1 << embs1.row(2).transpose(); break;
    case 12:  IN_embs1 << embs1.row(3).transpose();	break;
    case 16:  IN_embs1 << embs1.row(4).transpose();	break;
    case 24:  IN_embs1 << embs1.row(5).transpose();	break;
    case 32:  IN_embs1 << embs1.row(6).transpose();	break;
    case 64:  IN_embs1 << embs1.row(7).transpose();	break;
    default:  IN_embs1 << embs1.row(0).transpose();	break;
  }

  // Input Layer
  fixed_IN_errors = fixed_IN_errors * BN_gamma_in;
  IN << IN_embs0, IN_embs1, fixed_IN_errors;

  // First Hidden Layer
  // X1 = in_h1 * IN.matrix();
  for(int i=0; i<22; i++){
    for(int j=0; j<17; j++){
      X1(i) += (in_h1(i,j) * IN(j));
    }
  } 
  X1 = X1 + b1;
  X1 = (((X1.array() < 0).select(0, X1)) * BN_gamma_1) + BN_beta_1;
  // cout << X1 << endl << endl;

  // Second Hidden Layer
  // X2 = h1_h2 * X1.matrix();
  for(int i=0; i<20; i++){
    for(int j=0; j<22; j++){
      X2(i) += (h1_h2(i,j) * X1(j));
    }
  }
  X2 = X2 + b2;
  X2 = (((X2.array() < 0).select(0, X2)) * BN_gamma_2) + BN_beta_2;
  // cout << X2 << endl << endl;

  // OUTPUT LAYER
  // OUT = h2_out * X2.matrix();
  for(int i=0; i<49; i++){
    for(int j=0; j<20; j++){
      OUT(i) += (h2_out(i,j) * X2(j));
    }
  }
  OUT = OUT + bout;
  // cout << OUT << endl << endl;

  // Decision: NN_out holds the index of the maximum element
  OUT.maxCoeff(&NN_out);
  
  switch (NN_out) {
    case 0: MVX_HALF = -1;  MVX_QRTER = -1;		MVY_HALF = -1;  MVY_QRTER = -1;		break;
    case 1: MVX_HALF = -1;  MVX_QRTER = 0;		MVY_HALF = -1;  MVY_QRTER = -1;		break;
    case 2: MVX_HALF = 0;   MVX_QRTER = -1;		MVY_HALF = -1;  MVY_QRTER = -1;		break;
    case 3: MVX_HALF = 0;   MVX_QRTER = 0;	  MVY_HALF = -1;  MVY_QRTER = -1;	  break;
    case 4: MVX_HALF = 0;   MVX_QRTER = 1;		MVY_HALF = -1;  MVY_QRTER = -1;		break;
    case 5: MVX_HALF = 1;   MVX_QRTER = 0;		MVY_HALF = -1;  MVY_QRTER = -1;	  break;
    case 6: MVX_HALF = 1;   MVX_QRTER = 1;		MVY_HALF = -1;  MVY_QRTER = -1;		break;

    case 7: MVX_HALF = -1;  MVX_QRTER = -1;		MVY_HALF = -1;  MVY_QRTER = 0;		break;
    case 8: MVX_HALF = -1;  MVX_QRTER = 0;		MVY_HALF = -1;  MVY_QRTER = 0;		break;
    case 9: MVX_HALF = 0;   MVX_QRTER = -1;		MVY_HALF = -1;  MVY_QRTER = 0;		break;
    case 10: MVX_HALF = 0;  MVX_QRTER = 0;	  MVY_HALF = -1;  MVY_QRTER = 0;		break;
    case 11: MVX_HALF = 0;  MVX_QRTER = 1;		MVY_HALF = -1;  MVY_QRTER = 0;		break;
    case 12: MVX_HALF = 1;  MVX_QRTER = 0;		MVY_HALF = -1;  MVY_QRTER = 0;		break;
    case 13: MVX_HALF = 1;  MVX_QRTER = 1;		MVY_HALF = -1;  MVY_QRTER = 0;		break;

    case 14: MVX_HALF = -1; MVX_QRTER = -1;	  MVY_HALF = 0;   MVY_QRTER = -1;		break;
    case 15: MVX_HALF = -1; MVX_QRTER = 0;		MVY_HALF = 0;   MVY_QRTER = -1;		break;
    case 16: MVX_HALF = 0;  MVX_QRTER = -1;   MVY_HALF = 0;   MVY_QRTER = -1;		break;
    case 17: MVX_HALF = 0;  MVX_QRTER = 0;	  MVY_HALF = 0;   MVY_QRTER = -1;		break;
    case 18: MVX_HALF = 0;  MVX_QRTER = 1;		MVY_HALF = 0;   MVY_QRTER = -1;		break;
    case 19: MVX_HALF = 1;  MVX_QRTER = 0;		MVY_HALF = 0;   MVY_QRTER = -1;		break;
    case 20: MVX_HALF = 1;  MVX_QRTER = 1;		MVY_HALF = 0;   MVY_QRTER = -1;		break;

    case 21: MVX_HALF = -1; MVX_QRTER = -1;	  MVY_HALF = 0;   MVY_QRTER = 0;		break;
    case 22: MVX_HALF = -1; MVX_QRTER = 0;		MVY_HALF = 0;   MVY_QRTER = 0;		break;
    case 23: MVX_HALF = 0;  MVX_QRTER = -1;	  MVY_HALF = 0;   MVY_QRTER = 0;		break;
    case 24: MVX_HALF = 0;  MVX_QRTER = 0;	  MVY_HALF = 0;   MVY_QRTER = 0;		break;
    case 25: MVX_HALF = 0;  MVX_QRTER = 1;		MVY_HALF = 0;   MVY_QRTER = 0;		break;
    case 26: MVX_HALF = 1;  MVX_QRTER = 0;		MVY_HALF = 0;   MVY_QRTER = 0;		break;
    case 27: MVX_HALF = 1;  MVX_QRTER = 1;		MVY_HALF = 0;   MVY_QRTER = 0;		break;

    case 28: MVX_HALF = -1; MVX_QRTER = -1;	  MVY_HALF = 0;   MVY_QRTER = 1;		break;
    case 29: MVX_HALF = -1; MVX_QRTER = 0;		MVY_HALF = 0;   MVY_QRTER = 1;		break;
    case 30: MVX_HALF = 0;  MVX_QRTER = -1;	  MVY_HALF = 0;   MVY_QRTER = 1;		break;
    case 31: MVX_HALF = 0;  MVX_QRTER = 0;	  MVY_HALF = 0;   MVY_QRTER = 1;		break;
    case 32: MVX_HALF = 0;  MVX_QRTER = 1;		MVY_HALF = 0;   MVY_QRTER = 1;		break;
    case 33: MVX_HALF = 1;  MVX_QRTER = 0;		MVY_HALF = 0;   MVY_QRTER = 1;		break;
    case 34: MVX_HALF = 1;  MVX_QRTER = 1;		MVY_HALF = 0;   MVY_QRTER = 1;		break;

    case 35: MVX_HALF = -1; MVX_QRTER = -1;   MVY_HALF = 1;   MVY_QRTER = 0;	  break;
    case 36: MVX_HALF = -1; MVX_QRTER = 0;		MVY_HALF = 1;   MVY_QRTER = 0;	  break;
    case 37: MVX_HALF = 0;  MVX_QRTER = -1;	  MVY_HALF = 1;   MVY_QRTER = 0;	  break;
    case 38: MVX_HALF = 0;  MVX_QRTER = 0;	  MVY_HALF = 1;   MVY_QRTER = 0;		break;
    case 39: MVX_HALF = 0;  MVX_QRTER = 1;		MVY_HALF = 1;   MVY_QRTER = 0;	  break;
    case 40: MVX_HALF = 1;  MVX_QRTER = 0;		MVY_HALF = 1;   MVY_QRTER = 0;	  break;
    case 41: MVX_HALF = 1;  MVX_QRTER = 1;		MVY_HALF = 1;   MVY_QRTER = 0;	  break;

    case 42: MVX_HALF = -1; MVX_QRTER = -1;	  MVY_HALF = 1;   MVY_QRTER = 1;		break;
    case 43: MVX_HALF = -1; MVX_QRTER = 0;		MVY_HALF = 1;   MVY_QRTER = 1;		break;
    case 44: MVX_HALF = 0;  MVX_QRTER = -1;	  MVY_HALF = 1;   MVY_QRTER = 1;		break;
    case 45: MVX_HALF = 0;  MVX_QRTER = 0;	  MVY_HALF = 1;   MVY_QRTER = 1;	  break;
    case 46: MVX_HALF = 0;  MVX_QRTER = 1;		MVY_HALF = 1;   MVY_QRTER = 1;		break;
    case 47: MVX_HALF = 1;  MVX_QRTER = 0;		MVY_HALF = 1;   MVY_QRTER = 1;		break;
    case 48: MVX_HALF = 1;  MVX_QRTER = 1;		MVY_HALF = 1;   MVY_QRTER = 1;		break;
    default: MVX_HALF = 0;  MVX_QRTER = 0;		MVY_HALF = 0;   MVY_QRTER = 0;		break;
  }

  /* 
  Reset all values of arrays
  For Eigen library objects, reset is done by using object method .setZero()
  */
  IN_embs0.setZero(); IN_embs1.setZero(); IN_errors.setZero(); IN.setZero();
  X1.setZero(); X2.setZero(); OUT.setZero();
  array_e.clear();
  // NN_out = 0;

}

//end of modification


//! \ingroup TLibEncoder
//! \{

static const TComMv s_acMvRefineH[9] =
{
  TComMv(  0,  0 ), // 0
  TComMv(  0, -1 ), // 1
  TComMv(  0,  1 ), // 2
  TComMv( -1,  0 ), // 3
  TComMv(  1,  0 ), // 4
  TComMv( -1, -1 ), // 5
  TComMv(  1, -1 ), // 6
  TComMv( -1,  1 ), // 7
  TComMv(  1,  1 )  // 8
};

static const TComMv s_acMvRefineQ[9] =
{
  TComMv(  0,  0 ), // 0
  TComMv(  0, -1 ), // 1
  TComMv(  0,  1 ), // 2
  TComMv( -1, -1 ), // 5
  TComMv(  1, -1 ), // 6
  TComMv( -1,  0 ), // 3
  TComMv(  1,  0 ), // 4
  TComMv( -1,  1 ), // 7
  TComMv(  1,  1 )  // 8
};

static Void offsetSubTUCBFs(TComTU &rTu, const ComponentID compID)
{
        TComDataCU *pcCU              = rTu.getCU();
  const UInt        uiTrDepth         = rTu.GetTransformDepthRel();
  const UInt        uiAbsPartIdx      = rTu.GetAbsPartIdxTU(compID);
  const UInt        partIdxesPerSubTU = rTu.GetAbsPartIdxNumParts(compID) >> 1;

  //move the CBFs down a level and set the parent CBF

  UChar subTUCBF[2];
  UChar combinedSubTUCBF = 0;

  for (UInt subTU = 0; subTU < 2; subTU++)
  {
    const UInt subTUAbsPartIdx = uiAbsPartIdx + (subTU * partIdxesPerSubTU);

    subTUCBF[subTU]   = pcCU->getCbf(subTUAbsPartIdx, compID, uiTrDepth);
    combinedSubTUCBF |= subTUCBF[subTU];
  }

  for (UInt subTU = 0; subTU < 2; subTU++)
  {
    const UInt subTUAbsPartIdx = uiAbsPartIdx + (subTU * partIdxesPerSubTU);
    const UChar compositeCBF = (subTUCBF[subTU] << 1) | combinedSubTUCBF;

    pcCU->setCbfPartRange((compositeCBF << uiTrDepth), compID, subTUAbsPartIdx, partIdxesPerSubTU);
  }
}


TEncSearch::TEncSearch()
: m_puhQTTempTrIdx(NULL)
, m_pcQTTempTComYuv(NULL)
, m_pcEncCfg (NULL)
, m_pcTrQuant (NULL)
, m_pcRdCost (NULL)
, m_pcEntropyCoder (NULL)
, m_iSearchRange (0)
, m_bipredSearchRange (0)
, m_motionEstimationSearchMethod (MESEARCH_FULL)
, m_pppcRDSbacCoder (NULL)
, m_pcRDGoOnSbacCoder (NULL)
, m_pTempPel (NULL)
, m_isInitialized (false)
{
  for (UInt ch=0; ch<MAX_NUM_COMPONENT; ch++)
  {
    m_ppcQTTempCoeff[ch]                           = NULL;
#if ADAPTIVE_QP_SELECTION
    m_ppcQTTempArlCoeff[ch]                        = NULL;
#endif
    m_puhQTTempCbf[ch]                             = NULL;
    m_phQTTempCrossComponentPredictionAlpha[ch]    = NULL;
    m_pSharedPredTransformSkip[ch]                 = NULL;
    m_pcQTTempTUCoeff[ch]                          = NULL;
#if ADAPTIVE_QP_SELECTION
    m_ppcQTTempTUArlCoeff[ch]                      = NULL;
#endif
    m_puhQTTempTransformSkipFlag[ch]               = NULL;
  }

  for (Int i=0; i<MAX_NUM_REF_LIST_ADAPT_SR; i++)
  {
    memset (m_aaiAdaptSR[i], 0, MAX_IDX_ADAPT_SR * sizeof (Int));
  }
  for (Int i=0; i<AMVP_MAX_NUM_CANDS+1; i++)
  {
    memset (m_auiMVPIdxCost[i], 0, (AMVP_MAX_NUM_CANDS+1) * sizeof (UInt) );
  }

  setWpScalingDistParam( NULL, -1, REF_PIC_LIST_X );
}


Void TEncSearch::destroy()
{
  assert (m_isInitialized);
  if ( m_pTempPel )
  {
    delete [] m_pTempPel;
    m_pTempPel = NULL;
  }

  if ( m_pcEncCfg )
  {
    const UInt uiNumLayersAllocated = m_pcEncCfg->getQuadtreeTULog2MaxSize()-m_pcEncCfg->getQuadtreeTULog2MinSize()+1;

    for (UInt ch=0; ch<MAX_NUM_COMPONENT; ch++)
    {
      for (UInt layer = 0; layer < uiNumLayersAllocated; layer++)
      {
        delete[] m_ppcQTTempCoeff[ch][layer];
#if ADAPTIVE_QP_SELECTION
        delete[] m_ppcQTTempArlCoeff[ch][layer];
#endif
      }
      delete[] m_ppcQTTempCoeff[ch];
      delete[] m_puhQTTempCbf[ch];
#if ADAPTIVE_QP_SELECTION
      delete[] m_ppcQTTempArlCoeff[ch];
#endif
    }

    for( UInt layer = 0; layer < uiNumLayersAllocated; layer++ )
    {
      m_pcQTTempTComYuv[layer].destroy();
    }
  }

  delete[] m_puhQTTempTrIdx;
  delete[] m_pcQTTempTComYuv;

  for (UInt ch=0; ch<MAX_NUM_COMPONENT; ch++)
  {
    delete[] m_pSharedPredTransformSkip[ch];
    delete[] m_pcQTTempTUCoeff[ch];
#if ADAPTIVE_QP_SELECTION
    delete[] m_ppcQTTempTUArlCoeff[ch];
#endif
    delete[] m_phQTTempCrossComponentPredictionAlpha[ch];
    delete[] m_puhQTTempTransformSkipFlag[ch];
  }
  m_pcQTTempTransformSkipTComYuv.destroy();

  m_tmpYuvPred.destroy();
  m_isInitialized = false;
}

TEncSearch::~TEncSearch()
{
  if (m_isInitialized)
  {
    destroy();
  }
}




Void TEncSearch::init(TEncCfg*       pcEncCfg,
                      TComTrQuant*   pcTrQuant,
                      Int            iSearchRange,
                      Int            bipredSearchRange,
                      MESearchMethod motionEstimationSearchMethod,
                      const UInt     maxCUWidth,
                      const UInt     maxCUHeight,
                      const UInt     maxTotalCUDepth,
                      TEncEntropy*   pcEntropyCoder,
                      TComRdCost*    pcRdCost,
                      TEncSbac***    pppcRDSbacCoder,
                      TEncSbac*      pcRDGoOnSbacCoder
                      )
{
  assert (!m_isInitialized);
  m_pcEncCfg                     = pcEncCfg;
  m_pcTrQuant                    = pcTrQuant;
  m_iSearchRange                 = iSearchRange;
  m_bipredSearchRange            = bipredSearchRange;
  m_motionEstimationSearchMethod = motionEstimationSearchMethod;
  m_pcEntropyCoder               = pcEntropyCoder;
  m_pcRdCost                     = pcRdCost;

  m_pppcRDSbacCoder              = pppcRDSbacCoder;
  m_pcRDGoOnSbacCoder            = pcRDGoOnSbacCoder;
  
  for (UInt iDir = 0; iDir < MAX_NUM_REF_LIST_ADAPT_SR; iDir++)
  {
    for (UInt iRefIdx = 0; iRefIdx < MAX_IDX_ADAPT_SR; iRefIdx++)
    {
      m_aaiAdaptSR[iDir][iRefIdx] = iSearchRange;
    }
  }

  // initialize motion cost
  for( Int iNum = 0; iNum < AMVP_MAX_NUM_CANDS+1; iNum++)
  {
    for( Int iIdx = 0; iIdx < AMVP_MAX_NUM_CANDS; iIdx++)
    {
      if (iIdx < iNum)
      {
        m_auiMVPIdxCost[iIdx][iNum] = xGetMvpIdxBits(iIdx, iNum);
      }
      else
      {
        m_auiMVPIdxCost[iIdx][iNum] = MAX_INT;
      }
    }
  }

  const ChromaFormat cform=pcEncCfg->getChromaFormatIdc();
  initTempBuff(cform);

  m_pTempPel = new Pel[maxCUWidth*maxCUHeight];

  const UInt uiNumLayersToAllocate = pcEncCfg->getQuadtreeTULog2MaxSize()-pcEncCfg->getQuadtreeTULog2MinSize()+1;
  const UInt uiNumPartitions = 1<<(maxTotalCUDepth<<1);
  for (UInt ch=0; ch<MAX_NUM_COMPONENT; ch++)
  {
    const UInt csx=::getComponentScaleX(ComponentID(ch), cform);
    const UInt csy=::getComponentScaleY(ComponentID(ch), cform);
    m_ppcQTTempCoeff[ch] = new TCoeff* [uiNumLayersToAllocate];
#if ADAPTIVE_QP_SELECTION
    m_ppcQTTempArlCoeff[ch]  = new TCoeff*[uiNumLayersToAllocate];
#endif
    m_puhQTTempCbf[ch] = new UChar  [uiNumPartitions];

    for (UInt layer = 0; layer < uiNumLayersToAllocate; layer++)
    {
      m_ppcQTTempCoeff[ch][layer] = new TCoeff[(maxCUWidth*maxCUHeight)>>(csx+csy)];
#if ADAPTIVE_QP_SELECTION
      m_ppcQTTempArlCoeff[ch][layer]  = new TCoeff[(maxCUWidth*maxCUHeight)>>(csx+csy) ];
#endif
    }

    m_phQTTempCrossComponentPredictionAlpha[ch]    = new SChar  [uiNumPartitions];
    m_pSharedPredTransformSkip[ch]                 = new Pel   [MAX_CU_SIZE*MAX_CU_SIZE];
    m_pcQTTempTUCoeff[ch]                          = new TCoeff[MAX_CU_SIZE*MAX_CU_SIZE];
#if ADAPTIVE_QP_SELECTION
    m_ppcQTTempTUArlCoeff[ch]                      = new TCoeff[MAX_CU_SIZE*MAX_CU_SIZE];
#endif
    m_puhQTTempTransformSkipFlag[ch]               = new UChar [uiNumPartitions];
  }
  m_puhQTTempTrIdx   = new UChar  [uiNumPartitions];
  m_pcQTTempTComYuv  = new TComYuv[uiNumLayersToAllocate];
  for( UInt ui = 0; ui < uiNumLayersToAllocate; ++ui )
  {
    m_pcQTTempTComYuv[ui].create( maxCUWidth, maxCUHeight, pcEncCfg->getChromaFormatIdc() );
  }
  m_pcQTTempTransformSkipTComYuv.create( maxCUWidth, maxCUHeight, pcEncCfg->getChromaFormatIdc() );
  m_tmpYuvPred.create(MAX_CU_SIZE, MAX_CU_SIZE, pcEncCfg->getChromaFormatIdc());
  m_isInitialized = true;

  // EMI: Weights and Bias Initialization based on QP
  
  if(m_pcEncCfg->getQP() == 27){
    
    embs0 << 
			-0.37076148,0.14288566,0.04504776,-0.3031722,
			0.001458763,-0.006857113,0.008924945,0.037775304,
			-0.0011477413,0.008432277,-0.006366537,-0.01187675,
			0.004238676,0.051479187,0.036297802,-0.07397188,
			-0.0015890767,-0.020715125,-0.0147005515,-0.07626489,
			0.082178086,0.070841976,0.14805624,-0.2285758,
			0.0042186263,-0.012931041,0.12749071,-0.2344165,
			1.8707175,-0.17637654,0.0048760036,-0.24808519;
    

    embs1 << 
			-0.2976235,-0.26728636,0.19275591,0.16764817,
			0.0030818156,-0.01866567,0.035934668,-0.0059258817,
			-0.0027153657,-0.00014994989,-0.018547272,0.0047572847,
			-0.03005864,0.15593939,0.051988237,-0.0022025418,
			0.007597731,0.014053334,-0.08884741,-0.0064367275,
			0.12979373,0.18779112,0.050476223,0.02071992,
			0.10682189,0.05671229,-0.21103929,-0.13170631,
			-0.44545296,0.12594493,-0.5332202,-1.7132897;
    

    in_h1 << 
			-0.018540012,0.030114422,-0.0020006932,-0.017792078,0.032931507,0.03471266,-0.013559213,0.031002639,-0.23128216,-2.2490516,0.5991722,0.44812518,0.68774045,1.4211802,0.5117462,-0.8054585,-19.65846,
			0.064195976,-0.29960963,-0.13010803,-1.57507,0.07871229,-0.77200055,-1.4380281,-0.42455897,0.16233452,-2.4315045,-0.6437381,0.91947067,-9.078511,0.7789933,-0.43197575,2.1246493,-2.0718148,
			-0.047027,-0.051186137,-0.059248537,-0.046896856,-0.05109703,0.117740825,-0.071373686,0.07453218,-2.1170523,2.3499615,3.534245,6.373473,-0.23348649,-2.5857942,1.6723802,-0.33898696,-7.964443,
			-0.038132872,0.009646152,-0.021846501,-0.005698081,0.008726492,-0.1466241,-0.031042846,0.020712107,0.8312624,-0.6228588,-0.48820326,-8.836514,-0.34683323,2.6767476,1.8728546,0.1832476,-8.08721,
			-0.09605395,0.025236567,0.055567794,0.0003547996,0.06362298,0.21388687,-0.033334948,0.043505352,-0.08426489,16.29286,1.3296218,-5.6160765,-0.22834055,2.2137365,-4.9552274,0.21137987,6.3675504,
			0.0076208576,-0.023192255,0.0031950313,-0.25357,0.01592659,0.23092946,-0.2120229,-0.050017763,-0.33252513,22.275095,-0.43309173,2.8043668,2.2415187,-5.987887,-0.4439328,0.6819101,0.37174594,
			-0.08108047,-0.004668438,-0.07752809,-0.23136573,0.014567804,-0.6046118,-0.2537808,0.037988223,-0.5016481,18.54339,2.3925474,1.9022036,-3.3981397,-2.4071524,-0.46189678,3.2429972,4.794105,
			-0.009031256,0.027536148,-0.15809508,-0.07678087,-0.119687304,-0.7532702,-0.40427887,0.05939585,-0.13072036,-18.454103,1.1042435,5.097657,-3.9417427,3.306156,2.6464272,-1.7955519,4.022362,
			-0.04611515,-0.10890487,-0.014468444,-0.050592344,-0.054741032,-0.005601792,-0.01601528,-0.0060917824,-5.2663426,-8.3250265,3.7546158,2.9015706,0.63401186,-1.1873444,1.6633242,0.3655297,-0.6011025,
			-0.01935147,0.02326889,-0.116166875,-0.1993051,0.0010508728,0.03408608,-0.13004293,0.032237057,0.32213265,-10.966336,0.9304526,9.086519,1.2133504,-0.101497926,-6.3900576,-1.0807195,0.18229099,
			8.249555e-05,0.023656402,-0.047575876,-0.14768389,0.013045388,0.1271608,-0.10297854,0.02508942,-0.3932668,7.3986263,0.059924595,-11.230202,1.5205578,0.679741,0.16218796,0.16946337,0.61636454,
			-0.16278812,-0.21885617,-0.05468328,-0.18599993,-0.039675876,-0.049258508,-0.24739762,0.07314506,2.7151906,20.710388,-1.0335841,-3.118658,-1.5802735,-0.68820447,-0.018795023,3.084438,-1.9918118,
			0.24755959,-0.21555842,1.7010431,2.5352402,0.71270883,-0.050048485,3.0472653,-0.9498194,0.39861795,-30.514977,2.567375,1.9103923,-3.011183,0.85705304,3.061391,-1.7832894,10.620676,
			0.12798662,0.08310962,-0.18427542,-1.0072135,0.007641224,2.043875,-0.47722432,0.11862865,0.67100793,24.840399,0.36147836,0.91232675,-6.311067,1.15396,0.0074251746,1.4874235,-3.9978502,
			-0.056096047,0.025763806,0.010577153,-0.30910078,0.10266589,0.12324123,-0.22322921,0.109162144,-6.8151526,13.035885,-0.24559374,1.2112355,1.453577,1.1139065,-0.021447247,0.6598581,4.09632,
			-0.016829422,0.0022195578,0.00057811046,-0.052683532,-0.009112104,0.027271856,-0.020601822,-0.021066904,0.52168065,1.3608371,0.1097252,-1.0123099,0.69700974,-0.60075855,0.8589965,0.71459264,-17.879698,
			-0.018112227,-0.022796273,-0.010430849,-0.07899741,0.022715406,-0.14995329,-0.08597847,0.018145693,-0.019871702,37.108047,-0.12377116,-0.33759758,-0.1744312,-0.03772688,0.32018498,-1.2388886,1.4629197,
			-0.23201667,0.18036632,0.16853026,-0.1419558,0.11789777,0.50863683,-0.07386956,0.25318453,0.14491054,4.8943915,0.6514523,1.0251911,-10.008594,0.87717044,1.1175052,-0.7046251,0.80883753,
			-0.0282895,-0.0023476568,-0.020456262,-0.07816265,0.016512329,-0.03442808,-0.06548447,0.025690598,0.074865,-32.256687,0.1356105,0.04438926,0.1942602,-0.04805137,-0.048233688,0.69072485,-0.184796,
			0.012014085,0.008630005,-0.064528145,-0.285396,0.028756266,0.20958889,-0.18702011,0.03536634,-0.0030858996,26.870956,-0.054350935,-0.02636,2.6287289,0.056921665,0.21345922,-5.5172815,1.9894087,
			-0.17585918,0.025003735,-0.04969771,-0.06131252,0.08443056,0.058419254,-0.054747585,0.121476255,0.34494585,14.648457,-1.3409413,8.42773,0.104378104,-3.88003,0.28173175,-0.41955763,6.3879924,
			-0.1521062,0.014912373,-0.032823958,-0.11647697,-0.026102344,0.19346875,-0.07098178,0.1167758,0.25030676,-15.04866,-6.851016,-0.7710639,1.1352783,2.324538,1.3455874,0.46218866,3.070488;
    

    h1_h2 <<
			-0.41006052,3.2088192,-0.36203584,-0.25924084,0.6964866,0.6285465,0.55854946,-3.906885,-0.08446196,-0.5281308,0.22558668,-1.4827142,-2.812531,-2.7431545,-0.2343066,-0.46474278,1.0274199,7.254437,1.6376064,0.04945078,1.152878,-0.24458978,
			0.74114174,1.6869125,2.1074114,0.23227936,-1.0328971,-0.61366916,0.06794225,1.5465326,-0.8020024,0.71283674,-4.183412,-1.7433167,-2.706576,2.6544235,1.699393,0.65670717,0.40564987,0.30905148,-4.471882,-1.549481,0.11366377,-2.1385627,
			-5.9443417,1.2749026,-2.4014401,0.7487178,-0.58792704,-0.5604309,-0.45312956,0.04636796,0.5765263,-1.3875378,0.37001786,-0.4044109,-1.1887007,3.223024,-1.8555748,-2.856994,-4.4281244,-0.20542528,2.5475228,-2.3879762,1.5060587,-0.6823359,
			0.36903194,1.5671352,0.8688486,0.29010224,0.81031823,-0.26500237,-1.0643942,-0.27549806,-1.943307,-0.7843376,0.9805738,-1.5317496,-1.4601487,2.6831791,-2.2860665,0.84696496,4.2632155,-0.08644612,-6.59526,0.49733165,-0.23084407,-6.5661507,
			-1.9250847,1.4619694,1.4965829,0.6624038,-0.34972158,-0.12315896,2.3732738,1.5900013,-0.10232454,-3.7202175,-0.9401613,-2.5637355,-2.013007,1.7427536,1.3688623,-2.275374,-7.1479197,-0.25764686,-0.18168773,-1.1531405,0.9604462,-2.81674,
			-2.2301702,-1.4526486,-0.53054756,1.5599297,0.4120314,-1.6560191,1.331926,1.2256652,-0.90243024,-1.3987362,-0.7517237,-3.0072973,-2.3576777,2.0555675,-0.02173344,-0.5760154,-1.6354556,-0.61619246,-0.4858162,0.987205,2.997955,-1.4551948,
			-3.5110075,1.6060283,-1.371015,0.3305498,2.9287872,-0.39064544,-0.911138,-5.6700053,-8.631165,1.8477057,-1.1738876,-0.35169268,-2.4203968,2.0770962,-0.3252731,-2.1253147,-0.22809917,0.65311176,-0.20595594,-2.1808624,-2.0414445,-1.4133863,
			-1.6132472,1.5780444,-0.21391727,-1.9203613,-0.64482087,-0.064330444,-7.1288652,0.47731933,1.5828571,-1.1707584,-0.6955259,-5.290792,-1.2541045,0.33065256,-0.45882443,-0.7492549,-0.8615495,0.08668574,8.278812,-1.763041,0.42839748,0.07836478,
			-1.1600242,0.96491927,-0.24200453,-0.31611213,-0.8223163,-2.577921,-0.15646039,3.739898,-0.3699557,-1.3411641,-2.145337,-0.6198424,-2.2285662,2.0399284,-1.1080519,-1.1996007,0.36117226,3.629817,1.8166611,0.9900245,-0.9862606,-0.9331662,
			-1.9834951,1.5536804,-1.3500315,-0.9021759,0.9230354,-0.16421637,1.2944695,1.6352304,-1.341162,-0.57262766,-1.0231959,0.13465899,-1.8182385,2.3958473,-1.3121078,-0.48118457,-1.6593184,0.6274084,-1.8303711,-3.1188717,0.657017,0.40332058,
			-5.6625857,1.706496,-2.2309482,-5.249145,0.22135673,1.5955343,0.99279,-1.1887965,-4.803409,0.35199508,-3.2225583,-4.7060432,0.021589536,2.1398463,0.7437427,-5.1861672,3.4008744,-0.28450206,-8.027562,-0.49444166,1.6333823,-0.063673325,
			2.6108048,1.2950739,-1.3118265,-1.5934601,0.25509444,-4.101726,-6.0890827,2.4712894,1.6378137,0.7995208,-2.0478208,-0.51828134,-2.23089,0.6871603,-0.12604018,-0.31475106,-3.1325765,-0.13761581,3.518357,0.89297915,0.13349007,0.9638501,
			-0.55085886,1.1407026,-0.43621653,2.9977767,-0.28742477,-0.18668905,-0.102159455,1.4866928,-0.2626275,0.034679838,-0.3031972,-0.8767441,-2.3260713,2.0439334,-0.761778,-4.9460044,0.11307575,-0.26747388,1.7939858,0.7812632,-12.370212,-1.7037563,
			0.17370401,0.35281017,1.3339529,-4.3776245,-13.336781,2.02331,0.29794154,0.77328104,-1.5984591,-1.938037,-2.3264542,-0.32976514,-1.8788851,1.7830269,-3.2314055,-0.71141565,0.1370863,0.7547572,1.1287247,1.3355069,0.41429397,0.2425625,
			-0.13718179,0.8324238,5.292348,-1.5873495,-0.9737897,0.76236826,0.72128725,-2.2986352,-3.1242266,-0.54823446,-2.4133556,-3.2543926,-2.141959,2.1122105,0.48512632,0.6007246,-1.8860711,0.22756094,1.4662342,-1.5729306,-2.182811,-4.155232,
			-0.1317164,3.6050763,-0.68675214,1.0635376,-0.43089786,-0.22279543,1.1168681,-0.48067763,0.287034,-1.8122741,1.2440158,-1.1200358,-0.9755484,3.171994,-0.7370061,0.67011327,-1.4423198,0.26256454,-6.046198,-4.722348,0.4595223,0.04416364,
			1.5982087,0.90050143,0.88629603,-2.1450698,-3.2393582,-1.7198226,0.10404052,2.165606,-0.99937165,-1.096643,-2.729192,-0.032591343,-1.8447894,2.7100835,-1.5178207,-1.4131968,0.3506407,-0.7056994,2.368842,0.8627155,-0.18947898,0.74777293,
			-1.9825237,0.91029435,-0.1417601,0.92539746,-0.051341016,0.40657318,2.9260113,0.17858055,-0.19874623,-1.6423014,0.9331721,-1.3117306,-1.162311,2.849523,-1.2070354,-0.18763234,1.7291259,-0.017733598,-3.1798205,-1.2272186,0.85326904,-1.6640022,
			-1.3216784,1.7988821,-2.6809566,-2.827641,0.37795246,-2.5212407,0.15326598,2.999613,-1.0441381,1.4195049,-2.355559,-1.1220237,-2.6332555,2.473701,0.041046597,-0.29504126,1.2746884,-0.26230672,-4.6698203,-0.47016165,-2.3313673,0.42769855,
			-0.63087255,1.3189877,3.125844,4.5127964,-1.861004,-1.9163098,-1.204519,-2.2251153,-1.6249757,-4.82663,0.11804408,-4.616037,-1.5449618,2.3860025,1.5788007,-1.4051449,1.6216475,-0.22890224,-6.158026,0.7217079,0.16984697,-1.6236937;

    h2_out <<
			-0.29178163,0.27446875,-0.9369761,-3.601441,1.1705223,-2.2482233,-6.117128,-0.07323862,-4.194807,-4.3838,-2.541383,0.42153135,-1.0791229,-3.1387265,-0.4444792,-0.48740423,-5.3440566,-3.1662023,-2.482622,0.7714246,
			-1.1928562,0.8987136,-1.3429617,-2.7416086,1.9122554,0.98546124,-3.4969053,2.3033092,-0.8090906,-6.6208057,-4.5767894,1.3722022,-0.01453866,-2.7866933,-0.85759985,-1.4777728,-2.454748,-7.065672,-4.671163,-0.011682846,
			-0.58682454,-0.537993,-1.0084499,-2.5805204,1.353451,-0.22311544,-1.623622,4.0192246,0.7203525,-3.7518299,-5.178432,1.4459585,0.061128195,-2.2219915,0.20146184,-3.9216383,-0.32333726,-5.9883533,-2.795821,-1.3056664,
			0.43698636,-2.0834668,-0.026521394,-2.8709111,-0.057805732,-1.3345693,0.0139106475,3.3154397,0.7183145,-1.9747871,-4.3393316,1.0126663,0.13233913,-0.24016964,-0.5826322,-4.204757,0.016591586,-6.9041266,-0.32673514,-2.6256683,
			-0.97485304,-3.4189637,0.9607808,-2.459606,-1.8938397,-0.19833054,0.1980651,3.5201545,0.6975737,-0.37658107,-1.828706,1.8632869,-1.456817,0.07940468,-3.3099997,-2.9030411,0.18640906,-8.591885,-0.12400528,-4.917177,
			-1.6673392,-5.5063066,0.32283276,-0.8533449,-3.9637613,-0.060026832,-0.47886264,2.5617273,-2.8042562,0.61841744,0.056160033,1.3469948,-1.9968033,1.1196185,-5.2754207,0.6504828,0.55465174,-14.399461,0.29620624,-4.9167995,
			-0.93031,-5.561493,0.01769835,-4.102528,-3.6483748,-3.2062106,0.39548498,0.33404675,-5.9620934,0.3207951,0.70170635,-0.01649966,-3.3973792,0.9129184,-6.0707736,1.4530735,-0.79008263,-6.3341208,-0.6826949,-4.5312366,
			-0.78427124,-0.08422461,1.666161,-2.622931,1.2252957,-2.779567,-4.490242,-1.7618978,-5.3068066,-7.4423776,-3.135463,-2.5365813,-0.595,-2.8972797,-0.66683245,1.0248076,-9.929392,-0.4304272,-1.4304582,1.2380836,
			-0.7809982,0.61156523,1.0722839,-3.9654849,1.7442466,0.3826116,-2.0509248,-0.40102762,-1.9944512,-6.5157948,-4.57018,-0.040461037,0.95864636,-2.5626938,-0.9173609,-0.19733596,-4.0476837,-1.7287233,-3.6163762,0.6170319,
			-0.9893829,0.52036136,1.2008331,-3.1060107,1.7784433,0.0714338,-0.58717686,1.6393825,0.6709095,-2.5216842,-5.05974,0.2086082,0.44168386,-2.4586368,-0.21463665,-1.6750702,-0.901667,-2.533403,-2.895851,-1.4283806,
			0.2480565,-1.7926621,1.0980165,-3.5512042,1.3134472,-2.027899,0.47078833,2.0606878,0.834806,-0.3390342,-4.6780424,0.91958886,0.052248698,-0.41172016,0.41431698,-3.0054069,0.020561107,-3.5823977,-0.5376554,-3.3866773,
			-1.4110675,-3.7745168,1.100746,-2.8163116,0.31382692,-0.48211402,-0.16395657,1.5113584,0.3545706,0.7409287,-1.4298259,0.1974845,-1.6739849,0.5100879,-0.7820258,-0.61959493,0.5391269,-4.8421474,0.224386,-5.224088,
			-0.92478836,-5.341854,0.94111866,-3.6483092,-1.2221448,0.16682844,-0.65969956,0.49782947,-4.758349,1.045791,0.60896385,-0.61751914,-3.0992851,1.5014974,-2.7271488,0.4772732,0.16401774,-4.234282,0.26149917,-4.9606156,
			-0.8350436,-6.977436,1.4575597,-2.8729184,-3.230602,-3.0267534,0.52271795,-0.0055689514,-8.666404,0.76502013,0.66639733,-3.5488796,-4.933309,1.6390285,-5.728746,1.04378,-1.48248,-1.2648517,-0.93490344,-3.1279275,
			-0.2620227,-1.6409439,1.3427404,-1.1998987,0.19118044,-3.77428,-2.9776325,-3.1421418,-7.5515556,-2.281721,-3.432168,-1.3881031,-0.02498258,-1.6706287,-2.496313,1.6555986,-11.458256,0.29525265,-2.5745666,1.4605957,
			-0.8713848,-0.21688986,1.0784986,-2.0263991,0.43254036,1.092685,-0.49841845,-1.5393785,-2.7789488,-1.9996219,-5.058875,-0.89739424,1.0643115,-2.4351468,-3.18472,1.1490288,-4.715953,0.42075747,-4.3077955,1.2913431,
			-0.50653064,0.8606688,0.7408842,-2.181197,0.72568214,-0.025589973,0.51643944,-0.5190624,0.59301984,0.0036755488,-5.1760035,0.24219355,0.7139405,-1.9519695,-1.5740575,0.15576696,-1.5579003,0.4614122,-2.0683424,-0.31899476,
			0.7807103,0.11700766,0.20560163,-2.6343212,1.0444859,-1.8196688,0.17900716,-0.35642484,0.74727756,0.8304565,-2.7465909,0.35361546,-0.087920025,-0.63591266,-0.3397151,-0.011161993,0.1563008,-0.069528535,0.36881337,-2.6868913,
			-0.6271146,-1.8285363,-0.08717965,-2.3094113,0.77861154,-0.23502178,-0.47345495,-0.27544102,0.4721703,0.9087488,-0.11432813,-0.33262238,-2.367705,0.35655096,0.2518786,0.1897295,0.3364993,0.056363735,0.57774925,-5.260763,
			-0.9713718,-2.2662804,-0.107237294,-2.681538,0.44422194,0.4233541,-0.9061085,-0.5262255,-5.874115,1.1259958,1.1263078,-2.040321,-4.213488,1.8852397,-0.9488197,-0.0709636,-0.7611592,-0.25956634,-0.45182177,-5.402532,
			-0.79451865,-4.1603003,0.60284173,-1.4352041,-0.85489947,-2.6762602,0.1624691,-1.4805659,-12.214231,0.7913803,1.2838867,-2.013571,-6.366913,2.154684,-2.7896495,0.40034825,-4.0571613,-0.0822185,-3.3481724,-4.6321983,
			1.0747387,-5.305183,0.12175821,0.43747777,-0.7427041,-2.5380309,0.4727245,-1.9480183,-5.5675926,-0.24693634,-3.4413354,-1.4030304,0.36429393,-1.3863995,-3.3354697,1.1847792,-8.563685,0.15291154,-3.9660976,0.07652152,
			0.33619958,-1.9526336,0.64616644,-0.19068636,-1.5689563,1.3853652,1.1220777,-1.5742582,-3.2426965,0.20992218,-4.50668,-0.7493145,1.0167279,-2.0096867,-5.151996,1.1757685,-5.1322365,0.50083196,-3.2908697,1.1996164,
			0.49272326,0.36558703,-0.006156743,-0.6006951,-0.52466273,0.28000376,0.74079484,-0.8816834,0.6646685,0.7047108,-2.7993226,-0.87856096,0.6055697,-1.7909453,-3.5694978,0.75927836,-1.828651,0.7673078,-0.3366061,0.44977024,
			1.8749205,0.7520841,-0.8879412,-0.9460418,0.124316975,-1.954062,-0.20653087,-1.0500561,0.6866985,0.5811846,-0.81503713,-0.41349694,-0.3621426,-0.723313,-1.6534351,0.53235614,0.039105188,0.7160913,0.82086724,-1.1110674,
			0.503119,0.02616603,-1.3315147,-0.6444013,0.43174717,-0.04152369,-1.1281255,-0.5258624,0.5190951,0.6047939,0.790456,-1.6786665,-2.2024136,0.2609808,0.10838973,0.5766469,0.047814082,0.75922024,-0.16237101,-3.427607,
			0.32325467,-0.5840737,-1.8394963,-0.7179162,0.5279137,0.5256185,-1.6182157,-1.0968684,-6.6067214,0.17961627,1.1024041,-1.6974036,-3.955109,1.5913959,0.45209783,0.5283298,-1.0877157,0.65160733,-3.0688322,-5.6714225,
			0.76355326,-2.5803976,-1.383676,-0.07092955,0.10590912,-2.1654015,-1.4762985,-1.1052862,-10.378798,-0.28399092,-0.103339836,-2.4143765,-5.079208,1.719625,-0.3577923,0.7425416,-3.759644,0.45569783,-3.6170402,-4.4542427,
			-0.51583636,-6.84957,0.33070317,0.8908848,-2.7196517,-2.5550697,2.8932207,-2.5470617,-7.59998,0.2937682,-1.9841082,-2.149698,-0.33663094,-2.9382753,-5.657889,1.0518682,-5.397731,0.16568348,-3.3791912,-1.4154861,
			-1.1187176,-3.7939937,-0.24425958,1.1487515,-2.6731386,1.1508368,2.0163648,-1.4192786,-3.529957,1.2036921,-2.1199996,-0.8768172,1.054612,-2.1046371,-6.047287,-0.023008721,-4.14529,0.2499555,-0.9188285,0.07388761,
			-0.49506268,-0.5702461,-0.75417477,0.7316586,-2.7813008,-0.105124354,0.56150925,-0.8423536,0.55536264,0.9299354,-0.5625385,-0.19332477,0.554621,-1.5957386,-5.586016,-0.325846,-1.4726142,0.76565534,0.83464164,1.0741805,
			1.128595,0.60603696,-2.0786533,0.58316445,-1.5204833,-1.4036852,-1.055395,-1.8750181,0.83739984,0.01649969,0.77460724,-0.21217056,-0.17850336,-0.45840153,-2.9230022,-0.19959036,0.11259485,0.830336,0.93136024,0.44389677,
			-0.627068,0.86123496,-3.3770998,0.5171253,-0.525355,-0.2947989,-2.132486,-1.1612011,0.531809,-0.83631736,1.2193305,-0.56567967,-1.9054325,0.23168862,-0.7479695,0.42249122,0.43979827,0.83083695,-0.96569663,-1.2475127,
			-1.091602,0.68113035,-3.4176965,0.46058834,-0.33461425,0.27187327,-2.648929,-0.82959384,-4.73912,-1.4808303,0.34865338,-1.6722815,-3.6976197,1.4668032,0.5683035,0.4282034,-0.61904687,0.814198,-4.8644047,-2.9599073,
			-0.43746796,-0.6556052,-2.2572358,0.5348876,-0.30519858,-3.745778,-2.6987505,-1.3198359,-11.0606575,-1.6395242,-2.1441777,-2.686206,-6.7851105,1.383661,1.3090525,1.2853101,-5.013046,0.47372448,-3.559748,-2.7536142,
			-1.1676902,-5.9486704,-0.16306908,1.4040958,-3.2685473,-1.6727284,3.2481198,-2.1625028,-7.53491,0.7093526,-0.51385254,-2.4584181,-0.8864491,-1.8224145,-6.020835,-0.83496016,-4.2027926,-0.32842496,-1.2310631,-3.7616172,
			-1.1007792,-4.374827,-1.3588051,1.9133426,-3.79435,0.6704208,2.177933,-0.61087596,-3.9085255,1.2437377,-0.21020436,-0.10717462,0.9737142,-1.6436831,-6.5943756,-2.2135646,-2.8910685,-0.9148229,0.57499486,-1.7313732,
			-1.0072018,-1.0600586,-1.2754472,1.7122974,-3.924227,-0.08932525,0.4850304,-0.54441524,0.51974714,-0.22497343,0.85673237,-0.79095966,0.5661112,-1.6492468,-5.5678926,-4.29869,-1.1662636,-0.49138734,1.4124137,0.08758765,
			0.75826824,-0.25024885,-2.5845256,1.5488561,-2.8126383,-1.0498977,-1.7465641,-1.2738965,0.81714004,-2.7374537,1.1281204,0.21168387,-0.22607592,-0.23337159,-4.080774,-3.7399287,0.1532778,-0.14842638,0.79583424,0.91138667,
			-1.045346,0.851742,-3.4284837,1.6927016,-2.308576,-0.57400876,-3.057131,-0.6260867,0.18711998,-4.271025,0.34628338,-0.5346593,-1.7839406,0.4860565,-1.7299377,-1.3901368,0.67434585,-0.5611118,-1.2065074,0.4788871,
			-1.1746306,1.1942295,-3.7309413,1.6512502,-1.3577819,-0.32464924,-3.4117713,-0.629884,-3.9721391,-6.687993,-1.3735324,-0.22009414,-3.774341,1.067986,-0.15314655,0.8203515,0.017174272,-0.7478771,-3.9463263,-1.5099084,
			-0.89121574,0.65196186,-1.2622188,1.2525831,-1.4576288,-3.515688,-4.359537,-1.5067686,-6.9735646,-7.0875344,-3.4019933,-3.0385056,-5.717141,0.5641513,0.9496826,1.3671135,-2.9912522,-0.34024227,-4.7222066,-1.7561625,
			-1.0125717,-5.255328,-1.3776405,0.74167,-3.2149384,-1.845681,2.64963,-1.8939981,-5.18556,0.5528392,0.009744874,0.09504802,-0.9172525,-1.8876836,-6.6821027,-1.8186089,-2.797598,-3.3918552,-0.24621989,-4.136963,
			-1.4536257,-2.704415,-1.8213236,1.7648469,-4.5862393,0.8181033,1.9274919,0.3435519,-2.9012318,0.11895394,0.5243072,0.8384651,0.9013266,-1.2360606,-8.839191,-3.5801384,-1.5265496,-7.8122444,1.4251826,-3.3730571,
			-0.28656802,-0.23327373,-1.046783,1.5938654,-4.775611,-0.12834217,-0.046450127,-0.5247809,0.6580202,-2.0892315,1.1427597,0.93612397,0.4260001,-1.2728226,-6.0897117,-7.5038266,-0.94351727,-7.881097,1.424,-2.3232255,
			0.9417593,-1.2139117,-1.9854376,1.3090432,-1.5469253,-1.3145971,-1.494732,-0.76095575,0.5829076,-3.4679995,-0.30623403,0.6509469,0.10364334,0.15499052,-3.505334,-7.2273927,0.016919931,-3.8699868,0.37661368,-0.1304999,
			-0.3977246,0.298012,-3.3502574,1.9513856,-1.3480015,-1.5389528,-3.4031103,-0.6119381,0.28452262,-5.1434917,-1.6241844,1.3421347,-1.1827487,0.60775644,-2.0495377,-3.9091134,0.3772929,-7.0957303,-1.9928336,0.5637926,
			-1.5445112,1.0357147,-4.6378818,1.9682336,-1.6713759,-0.9083913,-4.407014,0.1997154,-2.3902218,-6.4077597,-3.1946194,1.3286289,-2.6544454,0.691489,-0.61477983,0.48518854,0.24433094,-8.709627,-4.837272,-0.53733885,
			-0.74393886,0.9051771,-3.0676432,0.6618675,-1.446459,-3.452983,-5.3200593,-1.0060431,-5.2873287,-4.383004,-4.2582135,0.17148574,-3.8791962,-0.09824875,-0.09902737,0.9324896,-1.1720462,-4.293177,-4.344808,-1.0895149;

    b1 <<
			-0.27313498, -0.55325335, 0.321083, 0.16307928, -0.2254296, -0.7986039, 0.5880809, 1.0639249, -0.21495238, -0.32690346, -0.4612306, 1.4255146, 0.5167373, 0.35907, -0.8772723, -0.2961034, 0.09745618, -0.6004021, -0.03187539, -0.4327464, -0.1105024, -0.43526778;

    b2 <<
			0.31854463, 0.8473582, 0.6033104, 0.42634878, 0.32705915, -0.23707852, -1.0590692, -2.2951758, 0.90510184, 0.7277509, -1.1123335, 0.28606185, 0.9359406, 0.9611106, -1.1426417, 0.9612291, 1.1618329, 0.86171126, 0.14468838, -1.3294147;

    bout <<
			-2.2011237, -1.5438523, -0.7085758, -0.37822962, -0.7384656, -1.5012729, -2.5873117, -2.6325393, -0.8447282, -0.016884059, 0.6518592, 0.08091284, -0.7381803, -2.8489094, -2.0577996, -0.44532758, 1.1646763, 1.5107995, 1.1984181, -0.5151061, -2.3980293, -1.7348001, 0.1904714, 1.5070153, 2.4389157, 1.4335734, 0.07396376, -1.803739, -2.5912588, -0.4920231, 1.1766818, 1.572765, 1.0904655, -0.5882343, -2.3403778, -2.687254, -0.72291446, -0.13805182, 0.6404744, -0.15705988, -0.90581703, -2.7292192, -2.442756, -1.5589033, -1.2111174, -0.49697724, -0.92451143, -1.7382747, -2.3558593;

    BN_gamma_in <<
			0.9785408, 0.033409536, 0.81636846, 0.32057822, 0.96985847, 0.9453109, 0.7068224, 0.9286743, 0.13554382;

    BN_gamma_1 <<
			10.284637, 3.2144744, 2.9512486, 4.3204436, 3.513, 7.0340223, 2.3291972, 1.2966805, 3.7615192, 5.3443494, 10.822831, 1.8366199, 1.5263633, 2.1309807, 6.929717, 9.644666, 6.380325, 4.498161, 12.12095, 8.131757, 3.681568, 5.1667504;

    BN_gamma_2 <<
			0.28799245, 0.15390445, 0.19231434, 0.17334849, 0.15918264, 0.21745458, 0.35712773, 0.41417006, 0.11312103, 0.14961173, 0.2556544, 0.17770393, 0.16662973, 0.20814535, 0.28038782, 0.17379348, 0.13579348, 0.11170353, 0.1739686, 0.23934;

    BN_beta_1 <<
			-0.07011686, -0.60554796, -0.7368163, -0.38705784, -0.06502767, -0.29953143, -0.5884704, -0.22829418, -0.046297956, -0.25615674, -0.28220338, -1.7621759, -0.02350703, -0.5214494, -0.24830152, -0.13528258, -0.39127538, -0.38808936, -0.1920398, -0.36507007, -0.096940555, -0.17750959;

    BN_beta_2 <<
			-0.26901484, -0.16853814, -0.2040636, -0.19730063, -0.17313886, -0.10256763, -0.12756595, -0.1619724, -0.08324692, -0.09974105, -0.09948471, -0.27307832, -0.20389642, -0.29147556, -0.07408557, -0.14361462, -0.10914674, -0.064840324, -0.0845861, -0.089166895;
    
    mean <<
      57479.15596430949,35502.85275136948,53428.18770928572,46543.56181859651,17589.15456102924,46240.91567667166,53692.6507076018,35119.45035165905,57276.29801354078;

    stdev <<
      207677.34728660225,154962.88030363916,191576.07600115417,181696.56921362053,127069.28025293918,180191.77552818554,192388.33261921056,154020.73044885873,205901.42822234138;
  }

  else if(m_pcEncCfg->getQP() == 32){
    
    embs0 <<
			-0.27660814,-0.15313397,-0.28480673,-0.23340121,
			-0.007274309,-0.05069594,0.0023311416,-0.0076166242,
			0.007425046,0.010647998,-0.0029277878,0.008904925,
			-0.019241838,0.05951499,0.106785275,0.035235368,
			0.006079973,0.08831304,-0.0029911266,-0.016813893,
			-0.17660324,0.19036295,0.24921039,0.16099636,
			-0.12944624,0.2184015,0.02063928,-0.032379277,
			-1.2489619,0.5443052,-0.53563976,0.43636587;

    embs1 <<
			0.22198531,0.29847595,-0.3723053,-0.17657065,
			0.038556337,-0.0014468202,-0.0060163434,0.028673248,
			-0.018061714,0.0025159684,0.0061778445,-0.00049456686,
			0.11909752,-0.04279286,0.009391266,-0.2228638,
			-0.08872193,-0.0065665785,0.0007013032,-0.030538574,
			0.09278248,0.16490369,-0.05056875,-0.25211906,
			-0.2099979,-0.017192088,-0.1685453,-0.085306875,
			-0.5918468,-0.29090944,-1.3892047,-0.12903082;

    in_h1 <<
			-0.09371817,0.18136111,-0.019074176,-0.04039412,-0.17090137,0.0057347817,-0.041804306,0.04996227,0.7246739,0.26735097,0.23856902,0.1060638,-24.035467,-0.13378811,0.47476363,-0.0021945375,0.11235882,
			0.009007855,0.14006925,0.007935462,0.03481381,-0.11531993,0.023747642,-0.00782437,-0.11705842,-0.27214995,0.2478052,0.2421179,-16.490007,5.7023044,2.3939438,-0.3105894,0.32452852,0.035069317,
			-0.6674164,-3.469996,-0.09861252,0.01866338,2.295045,-0.058322545,-0.5564184,1.547917,4.9071784,-5.661858,3.2486007,10.212195,1.0248536,-2.661484,1.5587666,-1.469477,1.4249783,
			0.071594626,0.12751728,0.08335271,-0.10820306,-0.033949252,0.027557783,0.052546587,-0.2532998,-2.224424,0.80630386,1.7919261,-3.829054,0.38719276,6.5901084,4.207038,-0.41497102,-7.4438477,
			0.10896892,0.3007219,0.1331606,-0.020567186,0.2933991,-0.07565864,0.011220771,-0.73069763,5.8566837,-8.554344,-0.5940324,-3.739884,0.45037863,0.57113695,-0.29502666,5.2741137,0.71410775,
			-0.013692849,-0.22827768,0.12972037,-0.04526862,0.6364118,0.081983075,-0.05085521,-1.0992861,7.5171623,1.1153603,-0.14007695,-4.139036,0.6488513,7.6528993,-6.5746818,-0.75916696,0.6523453,
			-0.050765127,0.3544521,-0.023194661,0.026868943,-0.55852044,0.04905773,-0.00071460736,0.061048627,0.28391632,-8.501691,-0.60104483,-0.7417252,16.156645,4.4160805,0.73566365,0.65719384,-0.50100327,
			-0.071485564,0.07852785,0.019948222,-0.009205541,-0.06941201,0.0373187,-0.066963255,-0.068803765,0.22703713,1.0354037,-0.42939597,4.069615,6.656067,-13.738582,0.28016368,0.63261503,-0.6821679,
			-0.057464577,0.092205755,0.055494837,-0.0021816047,-0.01966771,0.013316136,-0.08813798,-0.02530522,-1.0104,1.6073744,0.67997944,1.9885678,9.76924,2.6025507,-9.901692,-0.43865156,-0.028560981,
			0.0909306,0.09447203,0.076676585,0.01969484,0.31900102,0.07953797,-0.011286565,-0.543251,-3.9575357,6.834663,-0.12280125,-0.1388462,4.159362,-0.7024277,1.8204026,-7.1027846,1.2055519,
			0.011226755,0.18755843,-0.001449549,-0.1191249,-0.18356465,-0.16593613,0.07035577,0.09394873,3.684282,3.8566513,-0.16363491,-11.17158,-16.438707,-1.2505689,1.7463063,1.6154808,0.759352,
			0.08225403,0.022319322,0.08201283,-0.085555725,0.041663975,0.034120813,0.08110275,-0.054396246,2.819202,0.91969323,-3.3444262,8.4292555,-4.708712,0.14251466,-4.2140436,0.39181465,5.1673756,
			0.04108769,0.1250751,-0.023627073,0.008969067,-0.19237942,0.078836575,0.060049355,-0.03622897,1.4754525,-0.35006103,-6.2082376,-0.55774957,12.53729,5.6682496,3.0513353,1.084473,-0.74190193,
			0.047310058,0.007318241,0.08213801,-0.09404853,0.28954118,-0.0018943776,-0.0066659558,-0.4152487,-11.418585,-2.759175,2.5291133,5.422908,-0.22507554,-4.1261916,2.259549,1.9243269,-1.6104604,
			-0.072622545,0.15072231,-0.0048457426,0.00069194584,-0.1698845,-0.04147245,-0.04750548,-0.03983548,0.21991561,2.3414776,0.054705895,1.322298,16.72039,1.2205793,-0.30620188,-7.7731686,-0.26474714,
			-0.044682153,0.11993367,0.022136748,-0.05278418,-0.108465716,-0.057948932,-0.013285057,0.0017559936,-15.279347,0.86495167,0.027936075,2.739322,3.6839533,-0.067654595,-0.1288435,0.112313055,1.2684416,
			-0.04533864,1.0576135,0.004188329,-0.0031423352,-0.8178683,-0.16019271,-0.042028222,-0.5758088,-0.68244046,1.4298457,0.2844465,1.3927883,-23.625319,0.34639767,-0.1344676,0.9196079,0.37600207,
			0.09305659,-0.04252182,0.014510498,0.06534137,-0.027258933,0.072859816,0.076717734,-0.09924515,-0.16186257,-0.69359994,1.1786081,14.153373,-16.245354,0.13517275,-0.92168504,0.2979342,0.808001,
			0.059804987,0.0059202113,0.04063284,0.08388982,0.04620691,0.06218198,0.02146521,-0.3015635,-2.1074798,0.14204171,0.8900722,10.863533,-0.1584905,-11.516482,-1.012636,0.07264568,0.9316924,
			0.10563353,0.04304005,0.016603427,0.09987527,-0.14146015,0.1401752,0.058568038,0.0028176724,-1.8742783,-0.06455174,0.14999695,-12.5350485,-4.3922772,11.929164,0.36291885,0.118837185,0.3484372,
			-0.13865201,0.20581196,0.05775349,-0.018705033,-0.28040367,-0.02312793,-0.11928967,0.18343745,2.1687682,1.1713296,0.15207146,4.423584,8.04607,1.1974045,-0.9845789,0.82654136,-8.750443,
			0.09947106,0.35276994,-0.07599317,-0.027983345,-0.3503889,-0.13814741,0.12557338,0.17095089,1.9802853,-3.1658046,1.8799306,6.0602803,9.707759,-9.913172,-3.799985,-0.23144732,1.4756069;

    h1_h2 <<
			2.0674832,-2.4263039,-2.7207165,-0.7709444,0.548746,-0.38975292,0.49477977,-2.1655157,-1.617592,0.8866802,-1.3452992,-0.6571159,-2.0328765,0.22898641,0.726647,-1.4230301,2.501538,4.714027,-1.959585,-0.96012914,-1.7822379,-0.23086159,
			-0.015625363,-1.2448783,-1.2589179,-0.06791266,0.5542568,-0.18128757,1.0800705,-0.5585629,-0.344289,0.2678741,-0.5090156,-0.74143255,-0.5352489,-0.5135407,0.485921,-0.32171148,0.92503804,4.8978744,-1.2383974,-0.25639886,-0.21794926,-0.80381626,
			1.1215287,0.031019527,-1.6686217,-1.2999526,1.5279402,-2.5005383,1.1535258,-2.3410401,-5.035117,0.30610353,-3.8084643,1.8692532,-1.0190747,2.0876677,-3.138422,-1.0066025,3.1122139,0.14105646,0.34987974,0.47148502,0.5355498,-3.7336879,
			0.28575146,-2.7743518,-0.21672268,-4.769026,-2.4629974,0.34334403,-1.4037901,0.83856124,1.0692438,1.6916724,-2.4413655,1.309863,-0.5800296,-2.4853923,-0.16494915,0.11593206,3.068179,-1.4535524,2.1340413,-0.72805864,-1.9258364,-0.77496505,
			-0.09506956,0.19056211,-0.473948,0.5105499,-0.9682406,-2.4457734,-1.8419496,-1.1564025,-1.5583508,1.9222963,-1.2792788,-7.97141,-0.21524106,-0.19331878,1.2902443,2.6334689,2.609918,-1.3093357,-2.2153397,0.76468366,-0.3161202,-4.3603873,
			0.44234636,0.11654226,-1.6783205,-0.56076163,0.92430943,1.4977176,1.1504084,-3.0906909,-1.643518,0.195535,-0.943593,-1.916025,-2.593976,0.3494088,0.52809775,-1.3482336,1.7276073,-1.4649358,-3.8557487,3.4293888,-0.5839524,-1.6111351,
			-0.50170964,1.1896019,-1.337173,-1.599502,2.4395816,-1.0570709,-2.7004354,0.11382424,-0.7983215,-3.9893634,-1.3052952,-1.5426735,-0.6609929,0.98069906,-3.0690663,-1.002948,2.6370554,0.7048,1.4945369,1.0199252,-0.94674474,-1.2112415,
			1.5210309,0.32577518,-0.33296865,-2.2776651,-1.8419719,-0.38677102,0.28622323,0.8708763,0.6565625,-1.5024256,-0.97970945,1.0181293,0.0850884,-0.18170173,-0.0016680812,-1.191789,1.7237968,-0.85691226,-1.2503109,-0.91943496,-0.581711,-0.75530833,
			0.88651925,-1.5030493,-1.8590686,4.644603,-0.3811683,1.0376116,-0.6701709,-1.8896958,1.3205239,-0.29637724,-7.143718,1.7144041,1.1577619,-3.0691009,0.12376386,-0.23614852,1.3248322,1.1868082,-1.4192293,-0.341823,0.56653816,0.056050356,
			1.007858,-1.9748666,-2.0166845,-3.6512341,0.27483976,1.8003051,-0.72451705,-1.1359441,2.3339229,-1.7082651,-0.8079807,0.3799428,1.03251,-2.2887735,-1.493154,-0.27920154,2.276932,2.5170517,0.08510776,-1.0945452,1.2307874,-2.0802462,
			1.5575413,1.8242174,-0.58527327,0.17966886,-1.3993886,-0.659857,-3.1623476,-0.0039650598,-2.1111424,-0.057077173,1.2632303,-0.057632893,-0.086890735,-1.680014,-1.58555,-1.1370637,2.9971783,0.4898491,0.69857246,0.35703045,-1.0988644,-1.2175442,
			0.5665809,-2.1370752,-1.5957173,-1.5193969,-0.54585457,-0.037151497,-0.45879993,1.0903926,0.09355228,-2.295945,-4.122788,-7.0993056,-1.7093686,3.0416846,-1.1079693,1.8352991,1.6867757,0.2882368,1.200434,0.39289308,0.31603807,0.060997885,
			0.113089435,0.7677225,-0.5002568,-2.4687681,3.309848,2.930862,0.6201051,-1.5734072,1.1432755,-2.978019,-0.1786788,-2.4878943,-0.9472582,-4.339193,-1.7515634,-3.0385563,1.7653215,-0.22021775,-1.970855,-0.86492,0.4098056,-1.5594639,
			10.237869,0.40776142,-5.72177,0.12914988,1.9107668,-0.07967237,-0.54802454,0.6624167,0.12097689,0.32702228,-3.1052353,-0.7964068,0.09811171,-0.9695402,0.36699057,-0.23990747,-1.2407937,1.5987687,2.1361313,2.3737118,-1.3504558,-0.7911636,
			-0.4016826,-2.6331728,-0.34450185,0.57940805,1.0641258,-1.5923772,-0.31692007,-0.13660741,-2.615475,-0.0009280816,-0.4076413,0.2801379,-0.5614038,-0.6528355,0.94997555,-2.258133,2.2138498,1.7248131,0.1494787,-3.7706866,1.5428168,-0.99258864,
			1.0132701,-0.97868705,-0.5904914,-0.8279274,-0.36890596,-0.029354049,-1.1438802,1.0964599,0.2891119,-2.320976,1.0148774,-0.43684468,-2.7324228,0.13794719,-3.7764904,0.16510113,3.1928136,0.57848495,0.9493681,0.3318797,-0.82717437,-0.91895354,
			0.5214201,0.3170388,-0.8502143,0.8130808,-3.3554842,0.57892376,-0.5817355,0.6818253,-0.60759723,1.5455726,-0.19575912,-1.9566505,-2.486018,-1.5825249,-0.27285337,-3.4619539,2.1125324,0.67499226,0.624846,-0.51886666,1.1912233,-0.42210934,
			0.37542439,0.71526116,-1.2632849,0.29916954,0.5164024,-1.9070561,0.27935222,-0.12478448,-2.158968,-0.41252398,-1.461369,-0.6037599,-0.5961851,-1.1369869,0.36525953,-2.3603365,1.4949706,0.85453355,3.2083561,-9.844742,-0.9974638,-2.3969438,
			1.2399887,-0.8249189,-2.1666846,1.009828,-1.0979254,-0.17665368,-1.5688224,-1.2878134,-0.11268244,-1.229218,-0.36554876,-2.5637038,0.22751278,0.69486874,-1.440791,0.8428092,2.0326824,2.318324,0.32675767,0.60348725,2.1694808,-1.7187444,
			0.05949417,0.67512476,-1.3555255,-1.066338,-1.462388,2.7269664,-2.8128545,-3.482675,-0.45303074,0.40549973,-0.3821438,-1.1543853,-4.2303996,-1.3672829,-0.019109681,-1.4147173,3.6153316,0.7535164,-1.9159713,-0.18253174,-1.0807227,-0.1359575;

    h2_out <<
			-3.4739242,-4.6603866,0.76504415,-1.9516264,1.2857842,-0.59045684,-1.6757202,-0.35597554,-0.009859209,-10.875495,-2.5351927,0.7770543,-2.0532718,-2.6350477,-2.6193407,0.20274882,1.7666379,-1.8957384,0.075859405,-3.1665792,
			-0.9326652,-0.3111735,0.714648,-3.5243678,0.5290661,1.0122288,-0.939668,-1.0940193,0.8265336,-7.44429,-4.265127,1.7153795,-1.7678899,-2.7311523,-2.131442,-1.6122582,2.81989,-0.44993505,-0.35486946,-1.9437912,
			1.0062494,0.28641832,1.588697,-3.287813,-1.6072532,0.75414574,-0.9526965,-0.43044636,0.15710784,-5.3122582,-6.7074437,1.7751124,-0.346028,-1.4220244,-1.6428528,-2.1099334,2.3640127,-0.49036682,-0.55513114,-1.9147385,
			0.92501515,0.4291563,1.1681914,-2.6781752,-2.8387487,-0.2906358,-2.06966,0.727621,0.49858198,-1.6894772,-7.0253725,0.52999866,0.6321778,0.006005741,-0.27402112,-0.42660996,2.5464313,-0.40495896,-2.2151797,-2.26241,
			0.24106196,0.20170799,1.7213964,-0.9968674,-3.7761283,-0.93116015,-2.8576732,0.22437145,0.8632675,0.44225726,-6.1595783,-1.559409,0.8484542,-1.2068077,0.5832812,-1.8535404,4.0976787,0.8188548,-4.790396,-1.0744207,
			-2.0558312,-0.98376894,1.0517526,-0.55017084,-4.139544,-1.5589446,-1.9823958,1.0200887,1.349279,0.9968408,-1.6866405,-3.1512742,0.123248436,-3.1452892,1.5243002,-5.4096384,5.252734,1.0535579,-5.66957,1.2801343,
			-3.6913776,-6.4241753,0.94019455,0.24011992,-2.9367166,-3.8097246,-1.8823546,1.8920515,0.405748,0.67352307,0.4398292,-6.028377,0.2586563,-3.4557168,0.9267627,-3.9013538,3.4760873,-0.051105168,-5.0143414,-1.6265249,
			-5.4234686,-6.7400064,0.7209376,-2.813698,1.289474,-0.6439839,0.9534814,-0.22481449,-2.2314453,-7.8175817,-0.74071115,-0.82678825,-2.2949965,-3.2239327,-4.113799,1.0585271,0.57422423,-2.1159544,1.0923252,-3.445975,
			-2.402541,-1.0678393,0.08704815,-3.4768326,0.7319954,1.2297419,1.1332301,-0.6515453,-0.5943003,-4.5149264,-2.3420258,-0.036567178,-0.53992945,-2.5878575,-3.0220037,0.56153584,1.8685182,-1.2443622,0.8819853,-1.9243636,
			0.27631357,0.59481776,0.8330393,-3.482082,-0.9345687,0.61089677,1.2028145,-0.7703378,-0.93895066,-2.7333305,-3.4267936,0.43901685,0.78442025,-1.7834631,-1.6880506,-0.03784068,1.9632747,-1.269999,0.6249727,-1.2795722,
			0.75601983,0.6787055,0.91656995,-3.1043975,-2.8957086,-0.6502976,1.0559318,0.09086373,0.19823861,-0.26831546,-4.057829,0.82556623,1.1559559,-0.19408283,-0.38491586,-0.24228658,2.1716142,-0.80652887,-0.79069614,-1.8572322,
			0.13840695,-0.051598497,0.9008866,-1.1374717,-3.5518777,-1.8686935,1.0102262,-0.040036123,-0.11741984,0.9721373,-1.6953896,0.21836141,0.5307333,-1.922929,1.0286177,-1.1117891,3.1304445,0.48245606,-3.8681731,-1.3483434,
			-3.0855772,-1.8152142,0.07085594,-0.019637235,-3.099281,-3.077918,0.9596348,0.7182736,-0.06040361,1.1984371,0.2661111,-0.6304001,-0.5099555,-3.2320614,1.8172786,-2.3765674,3.8977597,1.0248704,-5.2259364,-0.76099175,
			-6.255533,-10.835364,0.46311784,0.011506151,-3.3069203,-4.7795897,0.30977198,1.8254887,-2.0998843,1.18217,1.1178063,-2.0001898,-0.2970845,-3.436499,2.1580129,-1.2831503,2.1572745,0.105746046,-7.5599885,-1.0135543,
			-6.3505692,-9.609527,0.13397881,-3.8575554,1.0322778,0.3003865,0.9084343,0.63204324,-1.7411219,-6.7664504,0.9667379,-2.490356,0.51750505,-1.9866673,-3.4294722,0.3933176,0.5835802,-2.1217058,0.78400135,-1.5972502,
			-2.7796762,-1.3336982,-0.41308472,-4.223349,0.77066,1.4194127,0.72057676,-0.34137484,-1.8079972,-3.855329,0.44887546,-1.9290471,0.90864307,-2.6831982,-3.5036404,0.75225675,1.2700512,-0.7288811,0.94608384,-0.7003186,
			0.34020934,0.6253677,-0.35976392,-3.8680778,-0.1471804,0.67551583,0.65151656,-0.5678368,-0.8037807,-0.86100703,0.07614837,-0.63481057,1.3307669,-1.398922,-1.99542,0.80168766,0.98717046,-0.98838454,1.076822,-0.9296484,
			0.6491667,0.7315816,0.06712622,-2.2550952,-1.8673807,-0.81961316,0.7302902,0.14649455,0.006635004,0.5570372,-0.050941255,0.1852427,0.48562306,-0.17391725,-0.28173345,0.6671841,1.2499813,-1.0416012,0.619793,-0.71618485,
			0.1890867,0.11360291,-0.27951074,-0.34514022,-3.5617328,-1.9426974,0.88428307,0.09650645,-0.03463182,0.94533783,0.54055315,0.74548715,-0.71169007,-1.7549167,0.7790325,0.28663746,2.1926472,0.54566157,-0.77083796,-0.7223179,
			-2.929518,-2.182066,-0.76444083,0.59223133,-3.9928782,-3.241222,0.7673951,0.34784877,-1.5562761,0.5749456,0.94030356,0.33190104,-1.7234993,-3.2465389,1.4621551,0.18316081,2.0142288,1.6369857,-2.8233588,-1.47157,
			-6.5433106,-10.52149,-0.49623427,0.6647905,-3.0591667,-4.550397,0.28638443,1.4059108,-2.735978,-0.087520055,0.6077016,-0.3569879,-0.95679766,-2.670343,1.9011372,0.76343185,0.97067493,0.7239788,-5.330479,-2.2198765,
			-6.4911804,-7.5208983,-0.71254313,-3.646425,0.5670104,0.17627728,-0.030891307,2.1489093,-2.615814,-3.3476233,1.1046977,-2.769386,1.179924,-0.7817698,-3.7113447,0.35772365,-0.58539355,-2.2142255,-1.1933452,-0.17374676,
			-3.822754,-1.1791304,-1.3048464,-4.0542917,0.7062193,1.3402233,-0.32545665,0.7624468,-2.2553167,-1.107705,1.1923859,-2.9455996,1.3899549,-1.316996,-3.6952605,0.33820474,0.042755347,-0.9528327,0.011383137,0.41545007,
			0.18971236,0.5859422,-0.97610265,-2.7027967,0.328385,0.71990985,-0.53771585,0.61883307,-1.4148095,0.5186379,0.9222577,-2.212533,0.60326463,-0.83860326,-2.1249747,0.7523072,0.03688879,-1.2387913,0.7144872,0.18498132,
			0.7219026,0.6113424,-0.7583762,-1.4214387,-0.4744024,-1.2907952,-0.48193848,1.2884685,-0.23101182,0.7640492,0.60651904,-0.5999604,-0.8996894,0.3641112,-0.2839324,0.66300476,0.07570845,-1.3066046,0.9593289,0.13896666,
			0.34998453,0.15219636,-0.80322593,0.29086554,-1.9164864,-2.0877461,-0.24272251,0.5443002,-1.4791188,0.39758173,0.9235011,0.37182242,-2.2533994,-1.104339,0.4290764,0.5701731,0.3026759,0.73584026,0.7635977,-0.626884,
			-3.3384979,-1.4156557,-1.4151396,0.8282636,-2.9387174,-3.357805,0.1425881,0.5513651,-2.0490537,-1.0105325,0.7611404,0.8762136,-3.20247,-1.938544,0.57340974,0.6670639,0.086739995,1.791575,-0.29492137,-1.9124663,
			-6.529736,-5.922857,-0.6818123,-0.385841,-2.065074,-6.045627,0.07582787,1.5999309,-1.7695414,-2.3840215,0.54767334,0.37414005,-2.4038315,-1.3095994,1.0438924,0.78150237,-0.5844713,0.5879127,-1.7899734,-2.183671,
			-6.5096016,-8.897133,-1.1505171,-2.4375293,-0.6038172,-0.13324918,-1.6347258,1.2909048,-3.4533052,-0.580929,1.1330802,-4.094271,2.011701,-2.07202,-3.8595383,0.48391065,-0.7820626,-1.5790095,-3.3365214,0.93015,
			-3.4249473,-1.1435784,-1.9564874,-2.1725626,0.28097537,1.1005892,-1.9383284,0.065705486,-1.5942503,0.49480563,0.73742527,-4.168069,1.2643125,-2.728956,-3.8285918,0.41492146,-0.64515406,-0.24333993,-1.9849148,1.4323314,
			0.08358767,0.7027713,-1.7286983,-0.7969715,0.8079089,0.5629573,-2.4264522,-0.19616874,0.062099844,0.8512977,0.887965,-2.996615,0.043200694,-1.4160066,-2.0604787,0.32825264,-0.7491015,-0.91590005,-0.91697806,0.88477707,
			0.77539814,0.665323,-1.0554639,0.24322273,0.5717804,-0.9824538,-2.213503,0.103254065,0.30171508,0.36931127,0.983297,-1.6885998,-1.994056,-0.044469062,-0.30940595,-0.2507166,-0.7348009,-1.0072081,0.16341624,0.5128001,
			0.3654787,0.2970938,-1.0491838,0.8343209,-0.5529904,-1.9781582,-1.6273695,-0.9894526,-0.17055935,-0.9871745,0.8520491,-0.15077177,-3.030602,-1.5626826,0.34753436,-0.116455935,-0.9418774,0.54985905,0.92213935,-0.5603608,
			-3.1048617,-1.8684374,-1.2306131,0.26755488,-1.4865844,-3.1061704,-1.0362854,-0.8574041,-1.7551415,-3.3537173,0.4241955,0.5997088,-2.8202326,-3.0146775,1.1229084,0.10924775,-1.1262649,1.6960262,0.8331963,-1.4288851,
			-6.763342,-9.952263,-1.0385327,-1.3848888,-1.2118737,-4.2152033,-0.65992975,-0.013028088,-2.8981621,-4.5485125,0.45364487,0.79951304,-3.5575747,-2.0063512,1.2534647,0.4384716,-1.2365128,1.2305995,0.50410914,-1.9805843,
			-5.950805,-7.7773185,-1.3224474,-0.9767552,-1.3064781,-0.8423902,-2.2778783,0.95082253,-2.0414257,0.8863875,-0.40455306,-4.828256,1.193286,-3.0407667,-3.4366179,0.80971074,-1.252369,-1.9943639,-5.8876605,1.4710064,
			-3.3815749,-0.5096246,-2.0212917,-0.2766889,-0.36990678,0.62367797,-3.5887747,-0.07967912,-0.020477625,0.9356897,-0.9813683,-3.2081592,0.70909554,-2.6872537,-3.0257313,-0.48506573,-0.9599431,-1.0488758,-4.3648434,2.0781567,
			0.31405845,0.6211747,-1.9246932,0.47385132,0.6346541,0.31356773,-3.310665,-0.5824002,0.08627153,0.7809453,-0.44166154,-2.9595332,-0.9147574,-1.7663217,-1.69613,-1.6665318,-1.0341103,-1.1567501,-3.8076193,1.6475556,
			0.95262945,0.48885852,-1.5500201,0.9299513,1.1314508,-0.8023726,-0.94868565,-0.3448198,0.7360893,-0.77069175,-0.076580316,-1.5619473,-2.476835,-0.1597494,-0.20729998,-3.9188883,-1.2116096,-0.8299727,-1.4625944,0.5848509,
			0.45839596,-0.09370579,-1.3682396,0.2389389,0.86286026,-1.8518445,-1.9119551,-1.3041734,0.6321152,-3.3677738,-0.3184623,-0.5598349,-2.2695856,-2.0213575,0.82731956,-2.8834229,-1.565908,0.5058975,-0.017170178,-0.2858834,
			-2.9161782,-1.4454641,-0.89886117,-1.1744765,-0.10196021,-2.5034742,-2.235024,-1.4738474,0.043771867,-4.005,-0.47476867,0.27080554,-2.3112547,-3.6297762,1.408104,-1.8217819,-1.6923363,1.0970427,0.9569736,-1.5544945,
			-6.882476,-7.304824,0.033446684,-3.368775,-0.43384084,-3.4246516,-1.4432563,-1.2087922,-2.585152,-3.6780894,-0.24107888,0.06828234,-2.6995468,-3.9467123,1.8240824,-0.33530936,-1.7798996,-0.19983228,1.0594001,-1.8705513,
			-2.9908433,-4.4222507,-2.2969668,0.17213733,-1.5310712,-0.74627876,-3.4562197,0.75676006,0.3997712,0.6960598,-3.069475,-6.7857337,0.97610253,-3.3026588,-3.0001838,0.53194696,-0.3999741,-1.3527688,-4.8419733,0.30198985,
			-1.6594782,-0.07309491,-1.8703701,0.41479477,-1.4231108,0.464436,-3.8283255,0.22933911,1.5907221,0.69891596,-3.5711274,-5.4569683,-0.096302405,-3.1675363,-1.6174039,-1.7510344,0.65850234,-0.033043213,-5.8821855,2.1121082,
			0.76069057,0.41362423,-1.6242138,0.984328,-1.0012753,0.33480683,-0.68248075,-0.15935738,1.6749924,-0.26054573,-2.1208267,-4.2248445,-0.97162867,-1.3974482,-1.1625146,-5.994411,0.22287339,-0.6671017,-4.3238792,1.6187167,
			1.2359347,0.14684375,-1.0303409,0.3807927,0.39080986,-0.54445016,-0.13333812,-0.056417536,1.0747738,-2.0266304,-2.0568235,-1.7710551,-1.7290146,-0.14767575,-0.019250846,-7.3540835,-0.7389819,-0.7021518,-1.9581647,-0.097725734,
			1.0931336,-0.3819809,-0.7310219,-1.0790712,1.031723,-0.882409,-0.007339569,-1.4425114,1.8255857,-5.2201953,-2.1878664,-0.1262422,-2.2358146,-1.4456571,0.68134034,-7.045462,-0.8616694,0.57507586,-1.0957091,-1.469098,
			-1.6171819,-1.2902774,-0.20157816,-2.9095538,0.55298895,-0.9404186,-2.7486174,-1.9965948,1.945176,-5.522789,-2.465115,0.47173646,-1.9916607,-3.5042474,1.4546912,-4.019899,-1.2167828,0.783179,-0.06718841,-2.7966664,
			-4.7775846,-4.917536,0.81581616,-2.7889252,-0.07454452,-1.6233189,-2.3239126,-1.2320491,0.5040395,-6.535909,-2.118114,-0.0074800923,-1.4078552,-4.4123135,1.6584626,-2.5452251,-1.3921052,-0.77818775,0.9805646,-3.1935096;

    b1 <<
			-0.14611636, -0.24098389, 0.3722656, -0.18210766, -0.007533249, 0.09504208, -0.27891606, -0.33478805, -0.4856142, 0.10502562, 0.31870762, 0.09295458, -0.4928757, 0.14481689, -0.3308282, -0.28453574, 0.17992945, 0.44832632, 0.043493867, 0.13102055, -0.6776937, -0.024125211;

    b2 <<
			-0.098517984, 0.90939724, -0.078733645, -0.76384467, -0.036112506, 0.022622041, -1.8448031, 1.7135073, -1.8656354, 0.12956831, 0.6493595, -1.197751, -0.12176402, -1.7076159, 0.8715349, 0.6339558, 0.98956454, -0.50235504, 0.6388952, -1.5316619;

    bout <<
			-1.8903737, -1.4240042, -0.8557587, -0.26914206, -0.7822777, -1.0787646, -1.5272535, -2.204591, -0.7512227, -0.28914535, 0.51369417, -0.06646064, -0.42285916, -1.8265067, -1.3841114, -0.4249962, 0.7023344, 1.2302794, 0.87754095, -0.2990054, -1.3885859, -0.90624166, 0.23358363, 1.12829, 2.2574055, 1.2007848, 0.28357187, -0.8508252, -1.5436784, -0.43992025, 0.69182485, 1.2292901, 0.69026333, -0.3853805, -1.3849189, -1.9945312, -0.75333226, -0.38318217, 0.5818709, -0.17867202, -0.5843908, -1.7641484, -1.9031405, -1.2425083, -0.69815683, -0.14988633, -0.6644349, -1.2536411, -1.5535963;

    BN_gamma_in <<
			0.19059473, 0.4622476, 0.85433686, 0.14374554, 0.13050616, 0.19418353, 0.41613108, 0.58521754, 0.59233516;

    BN_gamma_1 <<
			6.6056013, 9.47402, 1.2611476, 3.0985909, 2.8868124, 2.4354074, 5.5486474, 9.50069, 7.611685, 3.2941034, 2.753719, 2.3865998, 5.7506404, 3.033931, 7.3995614, 9.703662, 3.6612697, 2.0459158, 5.558668, 3.1028607, 5.7933574, 5.7153873;

    BN_gamma_2 <<
			0.15961027, 0.12115646, 0.19288999, 0.20317475, 0.21232465, 0.1926211, 0.1707037, 0.28326103, 0.21849987, 0.17157626, 0.14732015, 0.27434748, 0.19960861, 0.25049227, 0.17222789, 0.14705229, -0.19836819, 0.20601287, 0.17202671, 0.19071695;

    BN_beta_1 <<
			-0.6438283, -0.31997943, -0.044475462, -0.09957284, -0.17607616, -0.36011672, -0.58734924, -0.22278452, -0.15799022, -0.44754687, -0.40125206, -0.055882026, -0.1516667, -0.63344103, -0.30986404, -0.2306316, -0.7551842, -0.098823704, -0.3703227, -0.1743397, -0.29101387, -0.3144236;

    BN_beta_2 <<
			-0.08176396, -0.1026451, -0.26175886, -0.09504616, -0.14367904, -0.11798955, -0.08300258, -0.5319699, -0.14591542, -0.092266046, -0.10643275, -0.098602615, -0.113489084, -0.07168966, -0.2432504, -0.102120884, 0.24179071, -0.19629577, -0.13012452, -0.11543975;
    
    mean <<
      54923.98630026454,34253.43096171213,51066.430216003784,44663.03649485166,19008.86047394739,44379.26173907137,51312.456969054336,33936.88848975141,54689.80229219386;

    stdev <<
      203013.1651297521,152801.63182374722,187379.1033813536,178073.72926964832,128008.23973479208,176605.8923592302,188119.38609298086,151866.63960198764,200972.0251179253;
  }
  
  else if(m_pcEncCfg->getQP() == 37){
    
    embs0 <<
			0.32047316,-0.33997378,-0.22550249,0.16885343,
			-0.0029863138,-0.013834023,-0.01891849,0.05581167,
			0.0060063186,0.009951929,0.00881496,-0.0045007146,
			-0.3357313,-0.04563015,0.120068625,-0.08797524,
			-0.003343792,0.005939281,0.030244332,-0.12192897,
			-0.22215219,-0.24745372,-0.067990325,-0.34196234,
			0.015597044,-0.053215314,-0.09587421,-0.42632392,
			-0.2292819,0.7478406,-1.1892618,-1.0806168;

    embs1 <<
			0.036494106,0.16807488,0.35270008,0.29413512,
			-0.019672249,0.058203265,-0.0072883517,-0.013672037,
			0.015206055,-0.0068720123,0.0015971058,-0.008157835,
			0.011584636,-0.049590707,-0.039667908,0.43166497,
			-0.0008585807,-0.13391314,0.023795959,-0.00087295834,
			-0.02119127,-0.08722325,0.14217012,0.45585808,
			-0.11637267,-0.5106252,-0.05725115,0.019627957,
			-0.042564426,-1.3325152,-1.6512371,0.0845596;

    in_h1 <<
			0.18592665,0.050830428,-0.07249886,-1.3900944,-0.09517275,-1.2067714,-0.041380093,-1.2074065,-0.21971251,1.4717226,-2.3181894,12.742357,-5.650911,-0.72402555,2.2396522,0.50516766,0.4883387,
			0.011564296,-0.17227088,-0.089461245,-0.018869784,-0.18268275,-0.024106452,-0.13614005,0.021639362,-4.0467587,0.92082196,5.512034,3.7360861,-1.4804018,-1.843318,3.2546897,0.7412996,-4.1770954,
			-0.023148403,-0.025984135,-0.007886471,-0.094046056,-0.014957507,-0.0920587,-0.018787524,-0.06380629,0.0067236363,-1.1492474,5.297972,6.291734,0.24512258,-1.8072655,-8.181622,0.87033266,0.55664945,
			-0.018292358,0.11812777,0.045060694,-0.023584215,0.110466406,-0.022290373,0.05085117,0.09577849,-7.401771,1.9288156,0.40374503,10.205372,-0.48223287,0.34418356,-0.309653,1.1273568,0.9453637,
			-0.08280922,0.001615276,0.0152885495,0.3612019,-0.01223628,0.36635253,-0.039709795,-0.030220661,0.3376579,2.460987,-0.88638985,-12.556729,1.6802588,-0.4065778,4.9270782,-3.8856428,1.3698409,
			0.087589934,-0.005312434,0.1639616,-2.319361,-0.03249902,-1.8625461,0.21311969,0.5041432,1.7129346,1.4301392,1.8259013,-7.018865,-8.249495,0.91340077,0.09149195,2.9989061,0.05028558,
			-0.007805846,0.044238273,-0.0054400857,-0.013594543,0.04344955,-0.026042566,0.0034787273,0.056838293,0.7831042,-1.0102187,0.6049978,-4.0577426,-6.3893456,-0.92615473,0.78808707,10.064391,0.56165165,
			0.38962415,-0.06041372,-0.19101377,-0.12828687,-0.10123548,-0.3430635,-0.010100985,-1.3386487,0.39008644,-7.2059703,0.97480446,2.715451,7.1148195,-0.41244268,-1.5539044,-4.225459,-0.14789557,
			0.029971376,0.0420257,0.14692311,-0.3089768,-0.018250154,-0.20681356,0.092947535,0.28800982,-0.88749397,8.7072735,0.5589752,5.329142,-2.6722014,-0.6955477,-1.4336157,-5.6326437,1.6459283,
			0.4344911,-0.008109049,0.28117427,-0.43860483,-0.2753243,-0.19538686,0.117628396,-2.3857803,-1.6307418,3.393296,-1.574922,-5.191776,3.2637253,-3.0218544,-3.0789607,5.100597,-2.6188996,
			-0.07595638,0.0733855,0.05592813,-0.12796502,0.030248312,-0.046295974,0.101941064,0.4828705,3.1289303,-0.9003223,-0.6063719,-7.1378713,-0.710313,3.513034,1.5872725,2.2249405,-7.2828145,
			0.042466577,0.026658842,-0.0050635026,-0.07368206,0.05672694,-0.09246797,-0.014746637,0.02214862,0.35287756,-0.26626524,0.4255901,-17.071306,2.485126,0.8842477,0.18347394,-0.26702666,0.21915227,
			-0.011521581,0.04926913,0.16581285,-0.4244204,0.08572035,-0.28085864,0.071047634,0.17844859,0.20917179,-10.628515,-0.45712662,-2.78607,2.9284592,0.6355726,1.505978,4.5470867,-0.11762299,
			0.064773135,-0.00443957,0.01392203,-0.1288666,-0.017077211,-0.17413831,0.018477343,-0.7522372,0.8561385,0.23793936,-1.078034,-13.243783,-1.2786676,5.043036,1.0078355,0.71318907,-1.5667467,
			0.003578102,0.013232527,0.10507761,0.041155607,-0.0072310427,0.07849785,0.15361288,0.19226287,2.7654412,0.29077938,-6.36898,1.1038983,-1.1087587,4.1121054,-3.7507453,1.574811,1.9405968,
			-0.011862012,-0.0294934,0.0005047042,0.027457178,-0.015901526,0.020991739,0.04155749,-0.008065123,-3.7757976,4.505695,-4.1789675,4.320564,5.261183,0.09928104,-2.663603,-0.017096298,-4.4451075,
			-0.044350207,0.008583239,0.040410772,-0.28378448,0.051025067,-0.26798335,0.0010709286,-0.115332425,-0.12790841,2.73325,0.17404442,-8.266786,-9.037417,2.3934662,-1.742313,-2.0138538,0.8475543,
			-0.036304332,-0.014353035,0.2065588,0.11317298,-0.046694778,0.07489355,0.19774641,0.24012165,0.5780645,-5.5280466,0.95562774,1.0698215,-2.4838433,-0.81521326,-1.6364292,9.949426,-0.75342685,
			-0.09394325,-0.2543682,0.32862857,-3.69032,-0.09003432,-3.0182939,0.3688485,1.0447061,-1.2049898,4.410139,-4.9554477,2.101372,-5.273918,3.2446556,0.14111765,1.1204422,-1.413618,
			-0.0032531454,-0.0442053,0.02484002,-0.010396348,0.03285,-0.106091484,0.027648902,-0.029922035,0.31104502,-0.29197502,0.39166707,12.736849,1.6598153,-6.4316616,0.26083532,-0.78230983,0.82591254,
			0.17910543,0.088280946,0.44837034,-0.3023877,0.14300436,-0.17107412,0.4236584,-0.03974955,0.2609954,1.0253787,-0.5092375,4.782326,-9.82227,-1.272609,1.1201237,0.53968513,-0.71475804,
			-0.027906729,0.18769878,0.36285824,-0.5294318,0.14257495,-0.33168724,0.13647453,0.106186986,0.8109921,-2.8219426,2.031701,12.730496,-2.7702966,-0.673152,2.2511537,-2.8046265,1.4583559;

    h1_h2 <<
			-2.8291495,-5.6439276,-1.1947722,-1.02618,-1.7960911,3.649638,-0.42441034,-0.8368099,-1.677833,-0.553075,0.75576276,0.33122233,0.8195472,-0.61363536,0.2733862,-1.3140335,-0.47396702,-1.4334866,-1.43912,-16.87378,0.28857118,-1.6361146,
			-0.0917092,0.033933245,-0.55096596,0.057799246,-0.25903755,1.3420215,-0.61862636,-1.1497607,1.1395394,-0.7796658,-2.0511262,-1.1626798,0.07174651,1.9058952,-1.0078789,2.4090197,-0.036838952,0.6471882,-0.6282867,-1.1863978,-2.1145978,0.16007799,
			2.2428272,1.9508157,-1.51261,-1.3935105,-3.177151,1.1166413,-1.4357405,-1.0177181,1.0036637,-0.67628574,1.6444912,-1.3796405,-0.35386905,-2.3676984,-7.105437,2.2445734,0.11853684,-0.2163164,-1.5551001,0.1557422,-1.4548529,-2.9448621,
			1.6214192,1.1743013,-1.839613,-0.2560339,0.2327526,1.180648,-2.3094625,-1.8466079,0.7297012,-1.2698613,1.0748074,0.34491622,-1.6346111,1.997642,-5.6937637,1.302053,-0.46615687,-0.98843896,-0.77669734,-0.18577355,0.49830186,-1.4008778,
			3.5084186,-3.6762254,-8.065053,-2.4082077,-0.4080026,0.8559107,-0.11717805,-1.833784,-0.70463365,-1.1039681,-1.0365233,-0.22432704,0.21353018,-0.69850326,0.2663475,0.56022847,-0.4836237,-0.30661467,-0.7107624,-0.20657277,-1.0541713,-2.2557251,
			1.5343072,0.12630324,-0.5810063,-0.8805792,-0.72698766,0.26816243,-0.50677186,0.032303967,1.1163478,-1.7438018,-1.2634236,-3.5372994,-0.0408122,-2.4405527,-0.92859685,-6.1717486,0.22411901,0.865154,-0.1801726,0.43048576,-0.26022482,0.2436924,
			0.4300359,0.4830837,-1.4541208,-1.0776098,-1.6144713,2.5486443,-1.0108258,-3.0840683,-2.7654088,-0.4746788,-2.6248133,0.8160194,0.5856438,0.16724898,-1.5303699,-1.7665696,-0.0018165172,1.5479187,-1.4661403,0.6336919,-1.0775256,-0.4402499,
			0.64950037,0.8297736,-0.7958053,1.824469,-1.6401207,1.9264187,0.45919448,-1.280358,-3.2725291,-0.53695476,-1.3460798,-0.63572925,0.40104324,0.39282078,-4.2929773,2.4908693,-0.0987993,-1.9040482,-1.1293474,-1.6438259,2.2011793,-0.8735522,
			2.9990745,0.031175487,-0.95349395,-0.9207198,-1.7205019,1.123503,-0.6747111,-0.30541936,1.130022,-2.672467,-0.09457616,-1.3917133,0.025704509,0.16849469,-0.5969023,-3.3767736,0.25740972,0.39561814,-2.0167217,-2.6237757,0.27723148,2.6853197,
			0.61540544,2.1252778,-1.8915098,-0.26758236,-1.2536385,1.2222949,0.38957745,-0.79840255,0.5247517,-0.8607996,-0.56311435,-0.97821164,-0.5847843,0.26285318,-1.4812517,2.167432,0.8114404,-0.23124401,-0.9905807,-0.70628065,1.3837917,1.2136947,
			-0.15636809,-0.47253734,-1.3207313,-3.4819877,-0.07716959,2.302889,-1.795898,-2.5022488,2.4874218,0.119625546,-1.5738018,0.33055466,-0.11303001,-0.4968293,-0.8489981,-0.043402474,-4.7369113,-1.1520895,-1.1010838,0.5501283,-0.6365213,-1.0748475,
			0.57426786,-4.307995,1.8686057,-1.2531286,-2.4788554,2.2028742,-2.9222643,-1.4423846,0.5184173,-0.34528926,-1.392106,-0.091703184,-0.84426796,-0.109269544,1.0388538,3.413953,-0.16029106,-0.002295933,-1.2864581,-1.3425376,1.4230672,-4.959633,
			1.1575273,0.060242563,-0.53514105,-1.0101459,-0.33509815,2.2058964,1.2955623,-2.9751105,1.5111872,-0.21354888,-0.16995047,0.25673226,-1.9093872,0.07259094,-0.8697223,-3.8038878,-0.015540096,-1.4806176,-1.2693521,0.26330784,-0.24697646,-1.2554328,
			0.8346176,-1.986675,-0.44013873,-1.4385762,-1.8312926,1.0696176,1.9877687,-0.89600134,0.9722627,-1.0607768,-1.1678545,-1.3370975,-1.9445393,-0.380954,0.92029566,1.4485005,0.9291835,-1.9633614,-0.67701834,-0.5086162,0.9544762,-0.38874787,
			2.6370928,1.2208093,-1.0364219,-0.36402103,-2.1967661,3.5171819,-1.6145258,-0.38836703,-1.0812497,-1.2975813,-1.0154755,-1.304122,1.8637266,-0.084848695,0.32379282,-1.6037627,0.5970853,-1.5358298,-3.3817205,-0.6824745,1.105167,-2.2187335,
			1.3434954,0.10740171,-0.45814717,-0.45205504,-1.2640154,1.9290172,3.0232193,-1.0696752,-1.2560768,0.12355952,-0.42054278,0.02335043,-1.9746625,0.29531723,-0.68021625,-0.53663164,0.13618553,-1.8076216,-1.0545604,-0.02491394,1.158968,-0.89686084,
			0.49473,-2.19051,-0.16198589,-2.590602,-1.2616059,1.5481366,1.1816149,-0.98862183,-3.6943944,-0.45883706,-0.12636104,-0.6994935,0.34778976,1.2045457,0.8464913,1.4751714,0.1856528,-1.3597662,-0.86622214,0.14065918,1.0390506,-0.48097315,
			1.0575246,-0.23417875,-1.4262922,-1.2909005,-0.8889527,2.3047733,-0.66611946,-0.29191753,0.15410076,-0.22326902,-1.1181155,0.86880994,-0.4973577,0.021540344,-1.6278338,1.0116777,0.33237603,-0.6081951,-2.0295553,0.38086638,0.67383784,-1.0696999,
			-1.381032,-0.32073462,-0.9685657,-0.96707326,-1.0529803,2.8752546,-1.3411808,-0.53811246,0.056145154,-2.276415,-2.628213,-0.20020442,0.2617675,2.867221,-1.9729856,-2.7606869,-1.7365714,0.51321286,-1.104289,-3.4646597,0.09635772,-2.7442129,
			0.36777198,0.19505216,-0.16377507,-0.30800354,-1.07734,2.588417,-6.6100283,0.029048063,0.35108817,-0.64318746,0.650933,-0.662746,-2.460575,-0.539264,0.28425688,-0.3104256,-0.031514116,-11.131396,-1.2061683,-0.24273562,0.96679604,-0.5554509;

    h2_out <<
			-1.3014746,0.029216284,0.2248684,0.50337756,-0.20465454,-3.6797764,-0.12565683,1.8548716,-4.0356946,-0.482317,2.7881584,-2.495933,-2.2437596,-6.684232,0.57634175,-0.5877378,-6.182096,0.1648141,-0.73744684,-0.31397715,
			-1.5478384,0.2644356,0.31675276,0.3150434,-0.9020734,-1.0879819,-0.45694014,2.144697,-1.1638306,-0.40546805,2.103459,-1.8370132,-4.2386055,-7.142146,0.52339435,-2.0816617,-3.0924563,0.9562454,0.70442176,-0.2790153,
			-1.8123739,0.39070848,0.14563495,-0.8679044,-1.4775022,-0.83699757,0.08437795,0.96338975,0.09993363,0.4450428,-0.007885537,-1.558121,-5.8497424,-4.8003597,1.4274087,-3.1866822,-0.7893535,0.3225239,0.52341473,-0.6354422,
			-1.9284157,-0.1672374,-0.43785608,-0.91103643,-0.69098455,-0.42940107,0.41697982,-0.7288682,0.63347924,-0.40322617,-0.84183633,-0.8835738,-8.443902,-1.2586488,1.5262642,-1.5872936,0.059708755,-0.83839417,-0.3645506,-1.0157716,
			-1.7263408,-1.1643997,-0.9623983,-2.5838327,0.8382459,0.49030682,-0.3017527,-1.8676748,0.64411986,-0.9564612,1.1191771,0.39409798,-5.6760955,-0.5000153,1.2837819,-2.9469626,0.6588534,0.39464203,-0.86826664,-0.82092756,
			-2.8254082,-1.2890933,-0.83025366,-3.5037913,2.2982476,0.7846117,-0.91871274,-2.3761692,-0.27928543,-5.0932536,2.59979,1.7523189,-3.6140864,0.019868435,-0.11767795,-1.8026993,0.35839972,1.1218938,-0.48594078,-0.015451005,
			-3.0282595,-2.1323862,-1.8222373,-2.7589824,2.8408356,-1.0097657,-0.4172624,-3.068404,-2.8741653,-7.431859,1.0434147,1.9639922,-2.584398,0.3884929,-0.06532483,-0.12899561,-1.0695187,0.4930843,-2.0943766,-0.2898288,
			-1.2430283,0.036765892,-0.08227797,0.4283142,-0.053632557,-5.3766723,1.109544,1.4082114,-6.694812,-0.058089748,2.2794936,-1.4309571,-0.28480864,-6.841484,-0.12085572,0.1054784,-4.7342815,0.3778492,-0.5623137,-0.5711727,
			-1.2160252,0.7658971,0.017721666,0.36596042,-1.2134985,-2.27473,0.7611637,1.4505357,-2.4531133,0.05634085,3.1134264,-1.3005693,-2.0364938,-3.934176,-0.48724607,-0.21835081,-1.1967609,0.6952168,0.63163555,-0.37729537,
			-1.3405625,0.65553594,-0.06812199,-0.73818505,-1.2228206,-1.0633231,0.96488583,0.9506221,0.03549827,0.33029777,2.1339705,-1.2137915,-2.8952193,-3.1770992,0.23057123,-0.63401544,0.15462445,0.6988615,0.29405522,-0.647968,
			-1.7224025,-0.3199556,-0.13991338,-1.5740958,-0.54823637,-0.51192594,1.3378547,0.10410434,0.607155,0.11398126,2.3225107,-0.24367465,-3.6136122,-0.95804584,0.9930773,-0.86761093,0.7044757,0.23616503,-0.7398299,-0.59342504,
			-2.3135374,-1.2744508,-0.5030904,-2.4126658,0.8345734,0.47446874,1.0506042,-0.85568076,0.5913682,-0.6466592,1.885545,0.43254042,-2.7205427,-0.16551234,0.00047484212,-0.58953744,0.624959,0.7080038,-1.1964552,-0.712965,
			-3.6592977,-2.0494146,-0.683741,-2.3511677,1.9131376,0.79668885,0.8974539,-0.6047479,-1.0199742,-4.339599,1.5791322,1.3654405,-1.6251625,0.519149,-1.1275549,-0.2791695,-0.2228418,0.6511499,-1.9639335,-0.4881615,
			-3.3259764,-2.7831216,-1.3530736,-2.5002887,2.3339493,-0.98818624,1.113658,-1.4322815,-5.343431,-7.9594574,0.19492199,1.5544546,-0.40999705,0.30660272,-0.9249372,0.64050245,-0.86170626,0.16195823,-3.791329,-0.8571267,
			-0.9985851,-0.023150016,-0.77313066,0.47835752,-0.20558582,-5.711916,0.94381475,0.41057026,-7.06299,0.06989638,2.1080632,-1.6822573,0.32315162,-4.726781,-1.0479957,0.33551258,-1.8593944,-0.36377367,0.11143004,-0.32491392,
			-0.7893894,0.6369762,-0.80066186,0.58435786,-0.98533595,-3.0264058,0.44773155,0.63906986,-2.6917667,0.007024078,2.4045208,-1.3459519,-0.39145908,-3.1390924,-1.1846031,0.59929246,-0.14583667,0.18808296,1.019798,-0.5746786,
			-1.0190381,0.5930037,-0.6925242,-0.16713999,-0.93454254,-1.5094103,0.32587048,0.51624566,-0.014341014,0.32570162,2.3744555,-1.1159936,-0.65189284,-1.3205284,-0.12831105,0.5932141,0.5558568,0.45190766,0.37980738,-0.5734148,
			-1.8544633,-0.28324795,-0.23072112,-1.2319763,-0.41357297,-0.53188074,0.6955366,0.17961785,0.58466876,0.27546775,1.8278667,-0.42681822,-0.9246339,-0.12104713,0.6303545,0.6539449,0.45602396,0.23439872,-0.6269389,-0.6168514,
			-3.1399164,-1.4487681,0.02699758,-2.6772087,0.43600008,0.5410872,0.8128622,-0.29767838,0.3769853,-0.026357982,1.3393172,0.21903062,-0.6202948,0.43952033,-0.17076023,0.54034674,-0.2845552,0.4848917,-1.2763473,-0.5521246,
			-4.740867,-3.2094336,0.1114636,-3.1928482,1.239117,0.84881914,0.9682451,-0.45802692,-2.05656,-1.4064578,0.5652223,0.73616487,-0.21470821,0.41617534,-1.4055928,0.7000524,-1.172845,0.14182715,-1.9427277,-0.8302077,
			-3.7435536,-2.674542,-0.7216832,-3.2458506,1.2020952,-0.46251637,1.1554164,-0.79251945,-8.425908,-3.6130466,-0.21043548,0.19056168,0.10803781,0.44667184,-0.8609604,0.8611307,-1.4149692,-0.4281985,-4.2062693,-0.59981984,
			-1.0827862,-0.2284007,-0.9581248,0.03049644,-0.27519244,-7.2193685,0.3125215,-0.9995524,-7.6770453,-1.1691005,1.0810623,-0.69760984,0.16561565,-1.2532595,-0.17582092,0.5802103,-0.85735863,-1.2074825,0.5102103,-0.42527375,
			-0.73868966,0.3303922,-1.5288788,0.71892565,-0.8173984,-3.3874052,-0.56656736,-0.46671084,-2.5950956,-0.5453124,1.9351305,-0.2636139,0.24167086,-1.1290318,-1.1904211,0.6123866,0.49437866,-0.8172129,1.1602355,-0.69891644,
			-1.1861142,0.47691596,-1.2830999,0.37810054,-0.7483239,-1.8764517,-0.66340053,-0.1812035,0.0072652088,0.04163569,1.4664707,-0.39908406,0.042144455,-0.12296283,-0.12808013,0.7275024,0.36459258,-0.49688336,0.58383554,-0.5729009,
			-2.4840708,-0.23716934,-0.38320982,-0.47829476,-0.25677213,-0.7484497,-0.48727953,0.24296694,0.5427909,0.25753492,1.1964617,0.13102624,-0.025015214,0.42473096,0.50917053,0.77444994,-0.37946975,-0.8513792,-0.6567604,-0.30061063,
			-3.8703237,-1.8090479,0.29628205,-1.7147572,0.19274427,0.49333382,-0.014274048,0.30922025,0.18645915,0.27386367,0.4861344,-0.15300797,0.2234559,0.3842227,-0.141544,0.7028938,-1.7012305,-0.63947016,-1.5357231,-0.6996126,
			-5.270738,-3.4658208,0.6959339,-2.6496127,0.29881245,0.79434025,0.45410022,0.5027109,-2.9287624,-0.089205004,0.07870157,0.014763451,0.31155968,-0.101361856,-1.3514501,0.5655106,-2.161243,-1.1112531,-2.6090372,-0.7050693,
			-4.1538067,-2.5533786,0.06670484,-2.0726295,-0.11315089,-0.36603448,0.94815475,-0.056295946,-9.7969055,-1.2766591,-0.30033013,-0.901771,0.28149816,-1.3501631,0.17183733,0.69514644,-1.8213933,-1.5222799,-4.666035,-0.25485265,
			-0.67214715,-0.36840045,-1.9706107,-0.053798463,-0.76956654,-5.621166,-0.19219291,-2.233265,-7.4709744,-4.360447,0.2515778,0.6344942,0.7046926,0.27383468,-0.7951914,0.61810386,0.11544107,-0.69246554,0.13629812,-0.9794849,
			-0.89095134,0.5545207,-2.0565069,0.62602586,-0.9832677,-2.746682,-1.4482077,-1.4393755,-1.952891,-2.6467025,0.48555312,0.6201949,0.65898985,0.018999271,-0.9970542,0.29859015,0.5455735,-0.24851084,0.9302951,-0.90965337,
			-1.5921502,0.69906175,-1.7311013,0.65862393,-0.9017273,-1.3048038,-1.8143318,-1.1237228,0.26611713,-0.820614,0.28523323,0.2823991,0.50821465,0.4797529,0.044014793,-0.0364401,0.1803345,0.01988212,0.28255928,-0.19579431,
			-2.5092056,0.011154676,-0.922179,0.4032529,-0.28382528,-0.34932396,-1.622434,-0.31989697,0.49565658,0.13979931,-0.15324835,-0.07851533,0.40271628,0.5253642,0.6400342,-0.1562558,-1.0934068,-0.23640558,-0.8401032,0.22363812,
			-3.6524723,-1.2699096,0.22287518,-0.31308746,-0.09482596,0.5510792,-1.2425373,0.23945707,0.26183903,0.4997877,-0.31257722,-0.6750944,0.5653375,-0.2266514,-0.2773793,0.047866665,-2.6150749,-0.28713787,-1.6137805,-0.18058224,
			-5.027708,-2.936946,1.2157025,-0.97409016,-0.46564013,0.5372695,-0.52445906,0.43519774,-2.2947106,0.4062434,-0.36177096,-0.7955433,0.7320608,-1.397318,-1.1530536,0.22583452,-2.8457954,-0.58441806,-2.7070858,-0.88492537,
			-3.9755597,-3.2824218,1.1901407,-0.9781761,-1.329996,-1.1503261,0.46553254,0.10843492,-8.351107,0.32318595,-0.81409824,-1.6378322,0.5672977,-2.717868,-0.14017956,0.56299496,-2.2941017,-0.64044005,-4.916757,-0.74169093,
			-1.453676,-0.5810797,-2.0668097,-0.21854976,-0.8236461,-3.9690607,-0.33222422,-2.7234044,-4.5919027,-8.586718,-0.45784366,2.1003988,0.47345448,0.1113512,-0.45778036,0.5139531,-0.025011618,0.03796263,-0.51471627,-0.89863044,
			-1.486852,0.71216494,-1.9614125,0.4991167,-0.8737725,-1.6166972,-2.7365963,-1.4151973,-1.0685025,-5.4703712,-0.14387926,1.5719979,0.3352437,0.35280505,-0.3357126,-0.94713753,0.44145483,0.3759167,0.47143346,-0.42547852,
			-1.9581896,0.80804133,-1.7829628,0.45890853,-0.8560316,-0.7780854,-2.5310495,-1.6115,0.45956334,-1.7766731,-0.4588355,0.70894253,0.099369,0.53633153,0.0047216336,-1.6159576,-0.23369798,0.22978519,0.16141339,0.6729092,
			-2.6408043,0.08646808,-1.2694021,0.6980397,-0.41554394,-0.13428481,-2.1808872,-0.28133312,0.5613591,-0.23400848,-0.7026016,-0.32620963,-0.09106726,0.0042255004,0.20698325,-2.3685708,-1.035285,-0.6232578,-1.005506,1.1546988,
			-3.598924,-0.9908733,0.2774884,0.6588863,-0.5064315,0.39317238,-2.2651708,0.15612458,0.41489878,0.2665896,-0.6665606,-1.1641735,0.15882339,-1.6653173,-0.5338856,-1.625026,-1.9306096,-0.047112145,-1.5229024,0.7319916,
			-4.9354916,-2.363062,1.4503556,0.39422822,-0.9330123,0.13720739,-1.9065328,0.5406112,-1.6860708,0.3573516,-0.64608866,-0.9554342,0.25444233,-4.001345,-0.6573855,-0.82526314,-1.7375724,0.42699587,-2.390588,-0.03360862,
			-4.0543747,-3.4083598,1.965057,0.004282985,-1.2819978,-2.3383954,-0.4595408,0.41564605,-4.511137,0.07259142,-1.0450004,-2.0326853,0.33124188,-6.121048,-0.44030833,0.35507026,-2.6881893,0.21830787,-4.5343757,-0.18889299,
			-2.2727392,-0.77023613,-1.7960323,-0.709143,-0.9655415,-1.9480323,-1.587851,-4.0956135,-2.3866825,-9.780213,-0.28183985,2.5429955,-1.0727242,0.031515434,0.75890523,-0.31672254,-0.14313763,0.4980659,-0.42370248,-0.563124,
			-1.9516981,0.4384822,-2.2349987,0.16866924,-0.7285293,0.34016505,-3.5197663,-2.8305345,0.21944186,-8.574438,0.8380725,1.964321,-1.0493522,0.37524202,0.15114288,-2.3371832,0.15773328,0.47843707,0.5471751,0.46271208,
			-2.1865597,0.7386653,-1.9690443,-0.7961951,-0.95206416,-0.37702042,-1.8943602,-2.4737744,0.39071238,-2.2603726,-0.33733383,0.34352568,-1.5767003,0.54002535,0.2821927,-3.5980537,-0.2719686,-0.06880121,0.13218395,2.1177828,
			-2.832772,0.042763323,-1.3127426,0.5844088,-0.6257306,-0.16934446,-1.3518642,-1.0513895,0.616891,-0.7409186,-0.56432134,-0.831687,-2.2365441,-0.81440187,0.38713863,-3.4933467,-1.7004328,-1.2960143,-0.8555266,1.9795331,
			-3.281536,-0.90256774,0.14089707,1.2431215,-0.57276154,0.40504968,-2.2923138,-0.17815007,0.5612626,-0.15206611,0.15079898,-1.576895,-1.7775613,-3.8668368,0.13003758,-3.3209968,-2.9544432,-0.027070483,-1.3589689,1.9152557,
			-4.231923,-1.8913324,1.8613985,1.3173362,-0.6775023,-0.13460104,-3.3647711,0.6433126,-0.73678565,-0.32228038,0.1009192,-2.1011798,-1.7102804,-5.702842,-0.11090485,-2.1761413,-2.9595444,0.9312847,-1.1729925,0.79020804,
			-3.9077115,-3.0946338,2.225614,0.9425803,-1.2533612,-2.2630749,-2.118767,0.3572238,-3.2869794,-0.30637327,-0.17586575,-2.9387517,-1.2679439,-6.112386,0.3734082,-0.3812274,-3.4607904,0.69012207,-2.3713868,0.08532827;

    b1 <<
			0.44615957, 0.035940588, -0.0033125463, -0.32517543, 0.4188266, 0.43233606, 0.57413965, 0.2217051, 0.28677192, 0.082704216, -0.03856433, -0.30766138, -0.13385645, 0.42714027, -0.0048673954, -1.4416586, -1.4806567, 0.18190536, -0.48027438, -0.13689156, -1.1601584, 0.8057915;

    b2 <<
			-3.9312863, 0.98723465, 0.19504063, 0.47201222, 0.17059681, 0.25747296, -0.112243235, 0.69901055, 0.7334972, 0.6282409, 0.120162874, 0.036999896, 0.30120757, 0.30094072, 1.642537, 1.7140951, 0.77279276, 2.5186095, -0.20028755, 0.72107667;

    bout <<
			-0.94875556, -1.0735079, -0.5113992, 0.29809713, -0.6809944, -1.0727572, -0.8875338, -1.3404815, -0.5926285, -0.4024825, 0.544359, -0.3948492, -0.6260369, -1.4375519, -0.8355785, -0.52347714, 0.44017792, 1.0074403, 0.44828954, -0.6273971, -1.0224872, -0.060561974, 0.35325024, 1.0964433, 2.2556508, 0.9328992, 0.12341235, -0.20762306, -1.1556885, -0.6448977, 0.44045982, 1.1225089, 0.32519108, -0.78416693, -1.1148312, -1.5130903, -0.7527755, -0.41513458, 0.5895512, -0.46522802, -0.80728644, -1.5061798, -1.1365314, -1.1838735, -0.44002956, 0.34545213, -0.57444334, -1.1999553, -1.1687609;

    BN_gamma_in <<
			0.9416988, 0.7367732, 0.97308487, 0.28655952, 0.9890888, 0.99848294, 0.71001965, 0.77874297, 0.7748132;

    BN_gamma_1 <<
			1.1869209, 1.4338032, 2.200767, 3.1418664, 2.4946969, 1.334868, 1.2591257, 1.5685545, 1.4314164, 1.1497453, 2.695308, 5.0375614, 2.3687346, 2.3247566, 1.7769495, 21.479286, 5.939678, 1.7547437, 1.2068965, 3.564648, 2.5634282, 0.8539419;

    BN_gamma_2 <<
			0.45694357, 0.15156186, 0.22557764, 0.17937505, 0.2123526, 0.1633767, 0.17464171, 0.23061447, 0.12427675, 0.12745368, -0.17295504, 0.23681587, 0.1472677, 0.15163116, 0.23493688, 0.16511713, 0.17558762, -0.28167635, 0.19741699, 0.24158095;

    BN_beta_1 <<
			-0.55576956, -0.09011994, -0.16228643, -0.10755976, -0.89450675, -0.5054193, -0.117170915, -1.1223756, -0.26330265, -1.1509576, -0.1923609, -0.23442307, -0.40404046, -0.895982, -0.10779198, -0.041536905, -0.020499608, -0.038487624, -0.24962641, -0.18055217, -0.1563904, -0.25700372;

    BN_beta_2 <<
			-0.01941462, -0.19526808, -0.25092125, -0.15742113, -0.28842536, -0.08089348, -0.1122936, -0.26096106, -0.06103131, -0.06706005, 0.07691031, -0.2626334, -0.077489525, -0.05951607, -0.4166634, -0.3162093, -0.19167478, 0.71544373, -0.1080814, -0.42051786;
    
    mean <<
      50270.00278812122,32814.100678744784,46891.28361361622,41621.16404294983,21791.178170341536,41337.433591779336,47095.434124997926,32595.08412435691,49923.88279260183;

    stdev <<
      192277.57002833168,148902.63299216705,178409.20944839143,170471.77005005843,129693.55635497085,169053.99203804682,178924.76577600988,148103.8200182561,189676.17710192283;
  }

  else { // QP=22 and Default
    
    embs0 <<
			-0.39092818,0.044365346,-0.14316288,0.1886799,
			-0.0036663874,0.009940133,0.0033632214,0.031266842,
			0.005509182,-0.0058816853,-0.0023739545,-0.014351471,
			0.03204011,0.024617245,0.01877451,-0.07619405,
			-0.012279167,-0.013523536,0.0030431526,-0.07244116,
			0.18872027,0.43691316,0.054305177,-0.37640887,
			-0.006429432,0.13948657,0.029150708,-0.2357303,
			-0.0064332597,1.6714128,-2.1998792,-0.8348667;

    embs1 <<
			0.01078707,-0.23255458,0.25933418,0.07643056,
			0.006852757,0.002575908,0.006234502,-0.03448404,
			0.0035543975,-0.0028491153,-0.0047720335,0.018847127,
			-0.083408326,-0.004498381,0.007753046,0.0015373064,
			0.008145439,-0.0019062917,0.008035962,0.08963056,
			-0.11266643,0.081395864,-0.09245006,0.071729556,
			0.06630641,0.095629714,0.217076,0.26084685,
			-0.64628184,-2.22576,0.47357512,-0.5600457;

    in_h1 <<
			-0.015390737,-0.015555116,0.07792063,-0.07456693,-0.057052445,0.062276702,0.01673088,0.07460578,0.6164701,3.0496733,0.08833856,3.2817304,1.6797993,0.12602639,-0.14583291,-0.5553188,-8.179988,
			-0.04448683,-0.18065098,0.10544184,-0.60008126,-0.10186261,-0.00024826854,0.0090713175,0.53095806,0.78912383,4.3022823,-0.13000226,-1.1107093,-8.95068,3.8553686,0.61498237,4.0539484,0.18761063,
			-0.01841238,-0.062110502,-0.070245184,-0.14030221,-0.018380119,-0.091643244,0.0038296059,0.14560062,-0.2652369,1.7435389,-0.12241663,-13.405005,1.6816505,2.08799,-0.27380055,1.3635696,-0.56315166,
			0.031886853,-0.053225674,0.027770657,-0.08556964,0.020352172,0.017199492,-0.043814745,0.05317687,0.70583075,-10.015704,0.45879227,0.037797432,-0.016185578,-1.8622222,-0.8474796,7.5157795,0.022073094,
			-0.017173229,-0.027693965,0.099570036,-0.108506836,-0.023471633,0.07587995,-0.013703109,0.11529298,0.29084766,4.4543056,-0.16268416,-4.3191895,0.8607782,3.5488288,-7.1292205,3.2632504,0.07506381,
			-0.011955054,-0.03898729,0.013039751,-0.10127985,-0.024093367,-0.03943915,0.0024752177,0.11746322,-0.29674986,1.2919822,0.09974054,4.633548,0.8712609,-14.131219,-0.6003379,1.2403855,0.063039534,
			-0.0050927266,-0.059663,0.19408666,-0.10878741,0.013131472,0.13299572,0.007548235,0.07774424,0.5785806,1.1507431,-6.1441684,1.8356903,1.5233964,5.535357,1.3448403,2.9739664,-0.67584705,
			-0.044777628,0.17614335,-0.010747374,0.29481423,0.18323582,0.03293851,0.07001961,-0.30063376,0.19870922,4.20721,0.9756793,-0.081801794,-2.1599298,-10.849746,-0.25680995,7.8804326,1.43357,
			0.0034745315,0.0006187221,0.01525058,-0.006164952,-0.06302991,0.08245357,-0.029292908,0.013555085,-0.48320398,-0.15487467,0.3506563,-12.155208,0.20519432,9.045666,-0.47313604,-0.012039926,0.7579547,
			0.05188732,-0.88194996,0.10220453,-1.3415478,-0.31169644,-0.00044925348,-0.6016634,1.3332573,-0.7442135,6.82948,0.37582803,10.8797865,-5.584194,1.6699383,0.84776354,4.548593,-0.46085617,
			0.10238412,-0.0521531,0.0026424692,0.12237619,-0.088692375,0.022822531,-0.12044466,-0.040904928,0.6662714,-1.495241,-2.3618922,4.4031167,0.883387,-9.505541,1.1004164,-3.3235862,4.7428637,
			-0.067546465,0.056777034,0.05185101,0.034787968,0.11947819,0.00066911255,0.018880565,0.008173086,0.5570277,-0.67793566,-0.49047926,-6.1071754,-0.7227333,13.493239,0.9116919,-0.60794586,-0.72114885,
			-0.01860707,-0.05870008,-0.025674976,-0.10476517,-0.046809476,-0.03353862,-0.017981598,0.098337345,-0.25042418,-13.462829,-0.13649781,2.4213307,2.2081935,1.5946618,-0.4353548,1.3040996,-0.17755473,
			-0.02901411,0.032798134,0.0460773,0.039295774,0.40305927,0.0036534604,-0.06355194,-0.042146627,0.49505457,-1.0159703,1.5417137,-4.0332527,1.3624067,-3.8435483,4.1894965,-4.3710017,-5.582507,
			-0.0015278562,-0.043672998,0.0042596753,-0.05548966,-0.003549217,0.028270304,-0.033782035,0.022289835,-0.24674866,6.7794013,0.646625,2.6256762,0.8128273,-5.361032,-0.47258103,-8.966294,0.9123971,
			0.0056203566,0.011086605,-0.06356887,-0.059732,0.01937923,-0.009897869,0.015200462,0.04047536,-0.31286123,0.077701785,0.63139886,10.574502,1.5553285,-0.009007446,-5.974393,-3.4651616,0.33260652,
			0.0040169135,0.07660429,-0.090856045,-0.31875968,0.10098987,-0.003305643,-0.0039298246,0.2760935,0.006579508,0.7682753,0.077431455,0.16820778,-9.605353,0.8998918,0.30071184,0.44388503,-0.13572574,
			0.048803918,-0.19745576,0.19612345,-0.14473955,0.123003095,0.14607523,-0.15781589,0.1439141,-4.2689342,-7.331789,1.8159256,0.55271,0.088888496,0.29146287,4.012948,2.408324,-1.5278639,
			-0.016607583,-0.0143798515,0.027803244,-0.08959278,0.015469451,-0.016908484,0.011487715,0.068197906,-6.586483,0.9930775,-0.03983576,4.0115404,1.9411356,1.8663285,-0.30478272,1.632989,1.6206077,
			0.046240468,-0.14108026,-0.027941719,-0.20367944,-0.11264073,0.009111081,-0.074594215,0.16449626,0.63271093,2.200915,-0.4806722,-11.682599,-0.18400446,7.9089556,1.7412686,0.39382833,-1.5124993,
			0.12895304,-0.14815487,-0.09472212,0.2106572,0.06767808,-0.16984992,0.2648489,0.024573416,-0.4973004,0.20367901,-0.12270956,-0.27036795,-0.20099196,-0.50805444,0.18168506,-0.95007944,-0.23392428,
			-0.03710883,-0.16040282,0.042178776,-0.23869874,0.14347455,-0.053511173,-0.1212405,0.22648649,2.258926,5.590165,-1.4152972,-6.944951,0.11849217,4.904757,-2.4862723,-5.023635,3.2095993;

    h1_h2 <<
			-0.6521196,1.1630427,-0.71670115,-3.5033865,-1.4375693,0.41706923,-1.8418769,-0.6018716,0.3087354,2.8347332,3.089978,-1.4537609,-3.1569502,0.9108755,-0.016617185,0.2750956,-0.6199147,-2.944749,-4.7468514,-0.453397,0.0029238253,2.075631,
			-0.4954104,1.4671113,-2.019161,-0.10335854,-1.6273851,-1.9010161,0.63540655,-1.2259,-0.23982961,1.1107771,1.3062468,-1.2373767,-3.1453872,-2.8778875,-2.550825,-0.92246103,2.5808167,-6.583222,-1.1695212,-0.493686,-0.0004982665,1.8847522,
			-1.3679637,7.8980446,0.46864685,-2.7180703,1.3665068,1.4814516,-0.76382804,0.23479983,1.6669719,-1.8192072,1.6169246,-3.344074,-4.006005,-0.7405362,-2.2170815,0.857513,-0.008578579,-0.6526994,-1.2939711,-0.2585666,0.0030347914,-0.7821901,
			-0.7069388,1.5207905,-1.7933623,-0.67678714,0.6072141,2.2108297,-0.55334073,-6.097299,0.3881907,-1.6453407,-0.119146466,-1.6177303,-2.2118723,-1.1396648,3.014319,-0.21679766,0.44667017,-1.7366114,-0.9783409,-0.79922456,0.00048321587,0.008461747,
			0.16810979,0.84270763,-3.6719372,0.18615292,0.06900836,-2.3499484,-0.351193,-0.83432883,-0.92708504,3.3394384,0.1267658,1.9524417,0.33573234,-0.7568385,0.6536167,0.2542267,2.074543,-0.3091347,-0.13254485,-1.68706,-6.724715e-05,-0.20012477,
			-1.5144662,2.6551063,-0.33406872,4.601734,-1.7302676,-2.8437443,-0.3302436,-3.9149187,0.18947336,-1.9580408,2.0162737,-1.228917,0.10000795,-2.2575755,-5.711815,0.23693547,-0.49346638,-4.966563,-3.6412144,2.6411865,-0.00018372193,0.4127111,
			0.15704806,2.4510896,-3.1657217,2.6875882,2.220533,2.4725823,-5.4042845,-2.597001,0.9446052,1.0281923,4.2913985,-4.842056,1.0606292,-1.1794555,-2.2202792,1.9435589,-0.33572844,1.2231499,-0.19122368,-3.230992,0.0013065538,-5.8164654,
			1.1411141,0.19746244,-0.5563758,0.22521576,2.0458882,-0.42023462,-1.3916767,0.21687621,-0.21940497,1.7998712,1.2881488,-0.20139226,-0.5426751,-0.2989876,0.24471344,0.38668787,2.3285666,1.748981,0.2920631,-1.2447236,0.003050667,-1.0084534,
			-1.532868,0.78924143,-1.2192267,1.1869607,0.83554333,0.36854276,0.36799213,-0.9023967,-5.0080786,3.042641,-0.940494,-2.7820253,0.7064143,-2.2607863,-2.1719682,2.02482,-0.7882438,-0.11275824,-1.825268,-2.0911486,0.0015790339,0.16131306,
			-6.0209413,1.390905,0.37554651,-0.18732582,-0.6146114,-0.12950769,-7.399918,-0.8920672,-0.18399976,0.37134042,4.1890235,0.65721077,0.08386666,-4.154915,-0.07634097,0.6733867,-0.91085714,2.4627023,-0.44261533,0.6312894,-0.0013863562,1.2268413,
			-4.7424126,2.0081003,-0.28464973,2.884596,0.26799765,-0.2806528,-0.20664331,-1.6751145,0.67729837,0.6346428,-0.09738343,-0.9716333,-0.38520455,-3.0882494,-6.917769,8.888989,-0.22988835,-0.12555782,-0.6647651,1.5779178,0.0019170879,-1.3812052,
			-0.28157327,1.4959847,0.24699016,-3.4444401,0.42153266,-1.2625839,-1.0122687,-5.7239575,0.1809611,-1.2219158,1.348656,-0.55600744,-1.508619,1.322889,0.65269184,0.26456133,0.044573758,-0.7892992,-0.20300588,0.54390496,-0.00049195613,-0.948673,
			-0.33139908,0.43647838,-0.80870837,0.23472935,-1.732282,1.2353517,0.053040154,-1.1009033,0.4341598,-2.0529819,1.3255917,-5.0871305,-5.279769,-3.04038,-0.7673769,1.6893015,8.365687,0.12740727,-0.14415291,0.3833458,-0.0022747144,1.819373,
			-0.19607414,0.74843675,-7.362411,1.066784,-0.31880715,-3.1380026,0.6408663,-4.0797186,0.38668922,-0.8406184,-1.3149754,-1.579609,0.4143019,-2.0364742,-0.7759006,-0.62148994,10.492503,-0.48430797,-0.16734855,-1.9936025,0.0012670546,0.94961524,
			0.5262064,1.3090948,0.77715445,-4.9493475,3.9692895,-3.6678987,-4.0949535,-2.0368183,-0.09490182,0.88259333,1.9472655,-0.16479109,-0.95199853,-1.0365405,0.6652456,5.1741333,-0.23020563,0.07667833,-0.1428416,4.043542,0.0036943797,-1.6599047,
			1.4133722,1.7026287,-1.885272,-3.3957567,1.4292434,-1.5429909,-1.6451074,-1.1576794,0.16117449,3.1964982,4.1185517,-2.4441552,-3.2229776,-1.5320872,-2.597811,1.4649456,2.2602062,0.5805837,0.85742193,-0.6493602,-0.0021937825,-5.9538336,
			-4.4197636,1.7790078,-3.3579755,-4.7769213,0.180245,2.1125398,0.34789002,-1.4707896,-0.80083424,1.6233172,-1.1459397,-4.241742,-1.7731464,-4.4240212,0.992538,0.027582226,-1.4344394,-3.0973766,0.05567181,-1.6409487,-0.0015255472,-0.7051618,
			0.027037589,1.2616822,-0.26324424,-0.21980603,3.0305805,1.2124307,-1.5109392,-1.2804091,-21.962345,2.468579,-1.0294985,-2.308429,0.4454233,0.73457766,0.3997977,6.95363,-0.26353195,-0.91670674,-4.8156867,-0.5759227,-0.0036495787,-1.6146698,
			-0.94338816,6.752572,-2.5008519,0.6630361,1.0559938,-2.467041,-1.3345773,-2.5945013,-1.0955788,-0.35364076,0.7424811,1.9360433,1.2855906,-0.87351906,2.1195898,0.76211405,0.33188492,0.39733645,-1.518169,-3.3510716,-0.002619826,-1.7789209,
			-0.25459298,1.0036846,-0.70972157,-1.7415798,-0.48589948,1.4470372,-0.16843978,1.1208887,1.0328296,3.1254725,1.1913314,-1.6600688,-5.7100105,-0.925327,-1.6890147,0.6521025,2.083571,-0.9095616,-0.27558655,0.93458235,-0.0027621444,0.5392124;

    h2_out <<
			-7.129491,-7.5517826,-4.0132785,-6.839881,-5.0122824,-6.364198,-0.13159621,0.9093276,3.124549,0.08924705,-0.48849395,0.35569045,-0.23120378,-0.8406007,-0.15531349,1.7308649,0.33624506,-2.2122002,-8.85196,-3.0499394,
			-6.839535,-7.7406874,-11.168563,-3.233664,-1.558439,-4.324722,0.49836975,0.78425026,4.462968,1.6884923,0.15276965,0.62250483,-0.29493314,-0.9232009,-0.4383856,2.2699873,-0.5827269,-1.7441987,-2.0310228,-7.4912066,
			-5.948542,-5.456081,-17.441391,-2.8100538,-0.27291778,-0.74183714,1.5744677,0.75338215,1.2479295,0.8631989,0.0752357,-0.39813465,-0.34432825,0.8139344,-0.669209,0.79754215,-3.760678,-1.7400086,0.82051873,-9.812813,
			-4.4685144,-1.5995125,-14.82187,-1.7362251,0.38016292,-0.46368644,0.33828673,0.055640664,0.48559391,-0.4853279,0.40343574,-1.6472334,-0.63767385,2.0349371,-1.532791,-1.7335107,-2.7878876,-0.7210303,1.0240095,-9.254442,
			-2.3894048,0.06053193,-14.649355,-0.8307734,0.5892579,0.6105242,-0.9153809,-4.2107863,-0.8091196,-1.8564135,1.3843274,-2.4218621,-1.1276258,1.7237031,-3.6601455,-4.9804363,-1.513333,0.2863991,0.95239717,-8.232618,
			-1.8254942,1.178884,-9.155136,0.63379693,-0.32469565,-0.4648279,-2.075277,-7.797064,-1.4892007,-3.0120363,0.7193793,-2.672284,-2.0640292,1.5407543,-4.5437884,-5.831148,-0.5886217,0.51566666,-1.7400311,-4.2048655,
			-2.0450168,1.4770712,-4.6695333,-0.61825526,-4.187751,-0.8850936,-1.3738804,-6.81501,-0.74938583,-3.9590852,-0.13601509,-6.5446253,0.09376642,0.5173872,-2.527603,-8.425464,0.021508027,-0.3002775,-8.197838,-2.6089988,
			-4.0462146,-6.824279,-0.37150258,-5.02816,-9.537457,-6.620733,-0.3401829,0.7853152,4.4268036,0.7001655,1.4322803,-0.5368821,-0.42971417,-0.52203524,-0.0075548436,2.2951047,-1.0732998,-2.6429574,-16.464464,-0.14465405,
			-5.889075,-5.231271,-3.592512,-6.673596,-3.2158067,-2.05565,-0.15549405,1.0868223,3.4758072,1.7670798,1.5268786,0.3740473,-0.6306724,-1.2030393,-0.09407925,1.3558017,-2.3993795,-2.8828092,-5.6735797,-1.5961593,
			-4.4954433,-3.9524915,-6.3947787,-4.8580647,-0.21673867,0.3107085,1.0620178,1.0770236,1.2629355,0.8118133,1.9629561,-1.3574398,-1.1975303,-0.56150895,-1.6477319,0.38928327,-4.4137526,-2.692437,0.420901,-2.5429082,
			-4.403653,-0.9700793,-6.8767095,-3.4026666,0.44436485,0.8803682,1.5235173,0.7594199,-0.06501709,-0.666871,1.7775519,-2.2514381,-1.5855483,1.4538718,-3.648417,-1.1642013,-4.3183117,-0.9639523,1.0196449,-2.9324064,
			-3.7551913,0.95303875,-5.0935893,-1.7215594,0.3043595,0.060118306,1.4233172,-0.87943274,-1.1783165,-2.5474648,1.8449146,-3.341656,-1.7562298,-0.1814943,-4.8825502,-4.384932,-1.2521375,0.17195457,0.5175394,-2.3504028,
			-3.140174,1.3235586,-2.4221163,-0.019907696,-1.7678021,-1.3056511,0.3493466,-3.7087476,-1.801085,-4.4502635,1.1236459,-4.3539357,-1.1201292,0.028857358,-4.8531494,-6.1134105,0.36037675,0.72830164,-4.666977,-1.0118988,
			-1.4269257,1.4911187,0.15752137,-1.119491,-6.1307874,-1.9342121,-1.3974617,-7.279079,-1.2598151,-4.739741,1.2369307,-4.4332414,0.733011,-1.6047027,-3.5057058,-5.4572563,0.37657937,-0.21362986,-15.0408745,0.27576703,
			-2.1756885,-4.1540756,0.8851272,-5.9554963,-13.904936,-2.6489792,-0.8868165,-0.09363859,2.8010604,0.88582546,1.0178944,0.6430182,0.720593,-0.6880936,0.7252766,0.9610029,-3.7901337,-3.3167465,-17.896824,0.2285354,
			-2.8550286,-3.6423593,0.4277533,-6.003887,-3.6491294,0.5478749,-2.2640784,-0.34547743,3.5793388,1.9907128,1.5301087,0.31217554,-0.5951122,-1.6589806,0.81375736,0.69430095,-5.3017826,-3.480832,-7.32088,0.44354856,
			-2.7204127,-1.4680583,0.40039837,-5.8742113,0.39185837,1.1009839,-0.90510124,0.39764777,1.8806823,0.54573464,1.2073938,-0.516396,-1.3936268,-0.68008876,-0.51309437,0.43315944,-4.799771,-2.1743143,0.51335627,0.3640549,
			-2.4405575,0.40083012,0.47749376,-4.0136476,0.71817607,0.19760352,0.5734821,0.6541047,0.14730391,-0.5814683,0.8146477,-2.0038068,-1.4180341,0.93879384,-2.684148,0.061598834,-3.2435668,-0.9428634,0.97329956,0.014065392,
			-1.7082843,0.9419912,0.39534914,-2.017813,0.4220996,-1.4963894,1.7425274,0.04350247,-0.72151107,-2.195141,0.39977467,-4.1711063,-1.2284063,-0.059863392,-4.1554384,-1.9512248,-0.340219,0.19498505,0.5739161,0.22512385,
			-1.4184474,0.72055674,0.35768306,-0.46321827,-2.7129648,-3.3266642,1.4192619,-1.1277624,-1.4904258,-4.0095587,0.19432883,-5.4650836,-0.30261382,-1.3526648,-5.419228,-3.5038304,1.0762273,1.5647918,-5.713377,0.3406578,
			-0.78784746,0.24166688,0.97353864,-0.20534666,-9.085207,-3.347843,0.2046257,-3.8599725,-0.9673198,-3.346729,0.2554709,-4.8241587,1.726144,-1.8832375,-5.052347,-5.864766,0.9976275,1.1642989,-16.913307,0.38132507,
			-0.79572564,-0.82801974,0.801814,-5.9986596,-9.785227,-0.9211738,-3.5537508,-2.3642654,2.9767902,0.5381782,0.65827405,0.026422665,1.8082317,-0.6756555,0.21143587,-1.1967394,-4.1995335,-2.3855388,-15.622169,0.54577714,
			-1.109141,-1.0028183,0.9858503,-6.4539747,-2.9744837,1.0127122,-5.177559,-2.6727226,4.246637,1.7791997,0.8538032,0.9368627,0.9558785,-1.4730701,1.144915,-0.82295555,-5.6634326,-3.0908313,-8.812677,0.80243707,
			-0.4997574,0.33561715,0.7248269,-4.557683,0.5979993,0.12101969,-3.0034008,-0.73224086,1.8241937,0.472442,0.50834715,-0.2547803,0.14186665,-0.9125904,0.52367777,0.16269717,-3.3093278,-2.7135847,0.62607795,0.8848974,
			-0.25283757,0.5579445,0.71104366,-3.5908887,0.6641718,-1.7486559,-1.2229242,0.22737229,0.1716567,-0.5147634,-0.4174579,-1.27187,0.4375075,1.0403056,-0.8530134,0.44372457,-1.8198369,-1.002537,0.9686194,0.52107066,
			-0.2041945,0.3033571,0.70450336,-1.827965,0.18511498,-3.6641324,0.7540538,0.4628942,-0.208588,-1.8603696,-0.59714746,-2.9070175,0.3919958,-0.8702121,-2.9059691,0.28225082,0.472186,0.45330968,0.61243355,0.58632624,
			-0.5502298,-1.8782544,0.7292262,0.14879847,-2.7779374,-5.5248785,1.682345,-0.052290607,-0.40472227,-2.8369334,-1.3110062,-4.6983523,1.3112127,-2.1068082,-4.8942127,-0.9979753,1.1184446,1.9189577,-7.239647,0.66064876,
			-0.37801492,-1.4034883,0.7535071,-0.5639403,-9.662417,-5.61788,0.37849057,-0.31110203,0.070017844,-2.938168,-0.66004837,-4.5782475,1.4149998,-0.8514548,-3.8423927,-0.6330369,0.016736303,1.2621031,-15.41501,0.40884194,
			0.21870248,1.1947577,1.2513466,-3.9975185,-10.853272,0.45368946,-3.9554212,-6.623692,2.6240232,0.9094672,0.22290742,0.3131099,1.7092001,-1.5140486,-0.9149295,-5.4972157,-3.1710112,-3.393551,-18.401178,0.26731718,
			0.30058235,0.71520555,0.42857438,-5.0686793,-2.2733378,0.8043014,-5.251944,-5.8919225,2.979584,1.9108279,-0.84739035,1.246035,0.23558149,-1.6410882,0.7992484,-4.2210793,-3.0445554,-3.2446272,-5.800891,0.562287,
			0.3509959,0.7929352,0.47804502,-3.9138377,0.595612,-0.40519714,-4.8178453,-2.006036,0.79095685,0.6501305,-1.5328395,1.47464,-0.66664726,-0.0284126,1.1401869,-2.025243,-1.0730997,-2.7280595,0.6142465,0.4997005,
			0.48254928,0.34465355,0.64194757,-1.5581418,0.80735886,-3.558156,-2.8421857,-0.09692139,0.7864804,-0.76664424,-1.7719355,0.49110705,-1.023337,1.0944479,0.5781122,0.009293808,0.2010907,-0.9762075,1.0636666,0.22147104,
			0.35836992,-1.9926186,0.5958735,0.3552001,0.34190732,-5.608244,-0.27933472,0.5535807,0.9538877,-1.9873301,-2.0870266,-1.4598832,-1.170451,-0.5368677,-0.48516762,0.4692,0.92466164,0.5273258,0.54185355,0.24102607,
			0.4521086,-4.3966026,0.42609277,0.5015051,-2.7080808,-6.3614063,1.1809646,0.6430351,1.1408778,-3.5759313,-2.332262,-3.2646916,-0.74564624,-1.5544941,-1.8592088,0.7282509,0.67538613,2.112497,-6.7073092,0.31356183,
			0.07184624,-4.500131,0.76662743,-0.55933374,-11.881133,-7.367486,1.5389428,0.39789593,1.709217,-3.0350342,-1.2367126,-3.5456867,0.26302934,-0.3258505,-1.2645844,1.1910347,-0.5121202,1.8808135,-16.706497,0.31563652,
			1.2291136,1.137341,-1.6070776,-3.592391,-5.886298,0.3298231,-4.573714,-10.100555,0.50945926,1.0345746,-1.5025482,0.13641836,2.4680011,-2.575334,-2.0646982,-6.1269774,-1.8644648,-2.9268517,-14.52093,-0.5669387,
			1.5759404,1.3253887,-3.3253276,-3.4614074,-1.3653733,0.28496093,-4.9703636,-8.245505,1.1453253,1.6034776,-3.2858062,1.666094,0.3750682,-0.6324897,-0.4789952,-6.235843,-1.0591109,-3.1130679,-3.5880084,-1.4791206,
			1.6592655,0.7955406,-3.929983,-0.51736426,0.47077706,-1.9418547,-4.450149,-3.1017032,1.7697828,0.7594575,-3.0133622,1.739178,-1.0159225,-0.4850299,0.82221556,-4.6000714,0.17365699,-2.5337954,0.61391324,-1.8789788,
			1.3507042,-1.6815189,-3.8130689,0.8081617,0.63388526,-5.2043366,-3.485518,-0.5295443,1.5074567,-0.7621944,-3.0300975,2.0235536,-1.634107,1.627896,1.0053394,-1.985627,0.5461448,-0.92853713,1.1702875,-2.0311277,
			1.282611,-3.8508296,-4.2620063,0.9578506,0.20173071,-5.5233064,-1.8028852,0.40316993,2.6107826,-2.7436473,-2.8741937,0.97040606,-1.6530987,-0.8468125,0.84191775,0.18322748,0.28785393,0.72709554,0.5706708,-1.9594094,
			0.9351154,-5.8214893,-2.8771932,0.9335058,-2.2430606,-6.0207396,0.4108331,1.1029779,3.2517028,-4.431142,-2.611029,-1.107693,-1.2358223,-1.1572006,0.22931632,1.0296402,-1.0535189,1.840307,-4.071539,-1.4200776,
			0.62856394,-5.9057107,-0.34952518,0.33250776,-8.619414,-5.2566996,0.7731571,0.86724997,4.5679965,-5.1204877,-0.47694758,-2.3182049,-0.65124846,-0.9318984,-0.042769905,2.0253472,-3.9258351,1.590972,-14.019203,-0.14560764,
			0.5391552,1.6159626,-5.7110624,-2.6093073,-3.9801428,0.13412407,-3.823396,-7.6363826,0.74094546,-0.21959962,-2.8454742,-0.5143956,0.34160483,-0.38778284,-2.6263099,-8.470133,-0.71739227,-3.6317945,-8.144447,-1.997135,
			1.5897107,1.5273999,-10.956805,-0.7059835,-0.44658265,-0.6074684,-4.3827615,-7.917009,0.07994107,1.0754355,-2.9624743,2.6546571,-1.507558,1.0477531,-4.1899533,-7.2851615,-0.66638875,-2.9469934,-1.8202085,-4.498011,
			1.2204072,0.6977603,-9.226236,1.3114078,0.64642686,-2.4831538,-4.486566,-3.1818147,0.82845813,0.41963103,-2.9996114,2.440977,-1.7155584,1.0655906,-1.1431786,-6.012737,-0.028901005,-2.0889485,0.8707518,-7.2207747,
			0.3025851,-1.4847708,-10.868001,1.1104044,0.5710287,-4.7066517,-3.0131805,-0.6911558,1.4602137,-0.5267607,-2.913036,1.8404615,-1.9234664,1.6047423,0.20417051,-1.9502103,-0.6399335,-0.68176013,1.0858047,-6.1537137,
			0.38313028,-3.8538482,-9.471047,1.2262644,0.3220939,-5.9221206,-0.559364,0.56275517,3.097446,-1.6696734,-3.8374112,2.4847732,-1.7450144,0.11043605,0.62386,-0.46111795,-1.4540764,0.6294237,0.89997935,-7.803045,
			0.1469886,-5.8977227,-10.8074665,1.9545563,-0.44107002,-5.766164,0.028589888,0.6924863,3.5883746,-3.2019846,-3.1001198,0.7028736,-1.4156477,-1.2156606,0.45769635,1.9478945,-4.2751307,1.2695949,-1.5136473,-7.3084464,
			-0.8600496,-5.476142,-5.0140233,-1.9513917,-4.3701787,-5.2478943,0.2804994,1.0169386,2.4604583,-4.5136967,-2.3093708,-0.3664089,-1.461343,-0.8601518,0.11136082,1.3759934,-3.1370087,0.5001015,-6.9441648,-2.4531116;

    b1 <<
			-0.5984421, 0.120800644, -0.3263917, 0.2133771, -0.57420254, -0.22413866, -0.6317062, 0.507284, -0.11431619, 0.47713938, 0.19738343, 0.18364237, -0.26009887, 0.061215322, -0.02245066, -0.3089825, -0.8229769, -0.1772536, -0.62120444, 0.065517955, -0.45366696, 0.1465452;

    b2 <<
			0.6823686, -0.21775812, -0.27930486, -3.3910382, 1.4872262, -2.233574, -1.5841186, 1.5926414, 0.15967247, 0.81091744, -0.07360641, -3.2623456, -0.2701649, -1.6328653, -0.4912122, -0.33508635, -0.723981, 0.2590567, -0.8383529, 2.0551386;

    bout <<
			-2.9189289, -2.0281584, -1.604334, -1.2426707, -1.0471051, -1.1989877, -2.5756373, -2.654533, -0.77573365, -0.05938423, 0.6374793, 0.08159964, -0.49000242, -2.4080431, -2.5069814, -0.48644766, 1.1837133, 1.5412166, 1.40727, -0.15976451, -2.168421, -2.145105, -0.122293726, 1.2693172, 2.2334282, 1.3691604, 0.1910451, -2.0986598, -2.6125426, -0.39893246, 1.2996936, 1.5745304, 1.2273538, -0.4290329, -2.1884, -2.4663498, -0.6292059, -0.014509725, 0.67945915, -0.20803139, -1.0230004, -2.9414306, -2.8415265, -1.5926543, -1.1019442, -0.94888985, -1.4834279, -2.217102, -2.8660498;

    BN_gamma_in <<
			0.7534004, 0.18415612, 0.88642293, 0.16039705, 0.78575224, 0.14726889, 0.51944596, 0.23698235, 0.5079006;

    BN_gamma_1 <<
			15.026505, 3.4778476, 11.795571, 5.0983906, -9.100213, 12.347752, 9.017961, 3.8115778, 8.450255, 1.8414595, -2.5804262, 4.9940934, 12.742268, 2.907987, 7.7929525, -12.1888, 14.469146, 3.1772056, 13.465248, 5.8130217, 0.5417438, 3.081988;

    BN_gamma_2 <<
			0.17653182, 0.20331353, 0.17317323, 0.45618725, 0.12911986, 0.27961826, 0.28702828, 0.13996594, -0.18520653, 0.18953973, 0.20385161, 0.8188121, 0.26367164, 0.20961417, 0.19998519, 0.252634, 0.20798764, 0.21947436, 0.1619227, 0.13499686;

    BN_beta_1 <<
			-0.08842315, -1.0356584, -0.20590389, -0.8527328, 0.073391415, -0.20261875, -0.06302895, -0.9407472, -0.031485934, -0.16291627, 0.40228766, -0.29163036, -0.19366845, -0.61209756, -0.16161722, 0.05993226, -0.20345151, -0.1221594, -0.06290125, -0.5990998, 0.00014544142, -0.20312113;

    BN_beta_2 <<
			-0.16774309, -0.09384222, -0.036119457, -0.055136885, -0.18303291, -0.07179107, -0.104104824, -0.13979775, 0.1831587, -0.29491293, -0.23524147, -0.076708764, -0.197259, -0.18403167, -0.12142717, -0.098438054, -0.101864524, -0.25220624, -0.040947348, -0.19032295;
    
    mean <<
      58121.79697777398,35982.63326206248,54126.034579215026,47099.84770860291,16918.041715461684,46786.465602280994,54394.190536420305,35557.42198730395,57916.08224750105;

    stdev <<
      209719.99252336434,156429.86517925534,193617.6018444604,183553.46721868264,127286.93783731306,182093.47999310683,194486.93364851017,155514.52171421162,208118.4647262227;
  
    // inv_stdev <<
    //   4.76826262e-06, 6.39264119e-06, 5.16481968e-06, 5.44800387e-06, 7.85626567e-06, 5.49168482e-06, 5.14173359e-06, 6.43026766e-06, 4.80495568e-06;
  }
  
}


__inline Void TEncSearch::xTZSearchHelp( const TComPattern* const pcPatternKey, IntTZSearchStruct& rcStruct, const Int iSearchX, const Int iSearchY, const UChar ucPointNr, const UInt uiDistance, bool save )
{
  Distortion  uiSad = 0;

  const Pel* const  piRefSrch = rcStruct.piRefY + iSearchY * rcStruct.iYStride + iSearchX;

  //-- jclee for using the SAD function pointer
  m_pcRdCost->setDistParam( pcPatternKey, piRefSrch, rcStruct.iYStride,  m_cDistParam );

  setDistParamComp(COMPONENT_Y);

  // distortion
  m_cDistParam.bitDepth = pcPatternKey->getBitDepthY();
  m_cDistParam.m_maximumDistortionForEarlyExit = rcStruct.uiBestSad;

  if((m_pcEncCfg->getRestrictMESampling() == false) && m_pcEncCfg->getMotionEstimationSearchMethod() == MESEARCH_SELECTIVE)
  {
    Int isubShift = 0;
    // motion cost
    Distortion uiBitCost = m_pcRdCost->getCostOfVectorWithPredictor( iSearchX, iSearchY );

    // Skip search if bit cost is already larger than best SAD
    if (uiBitCost < rcStruct.uiBestSad)
    {
      if ( m_cDistParam.iRows > 32 )
      {
        m_cDistParam.iSubShift = 4;
      }
      else if ( m_cDistParam.iRows > 16 )
      {
        m_cDistParam.iSubShift = 3;
      }
      else if ( m_cDistParam.iRows > 8 )
      {
        m_cDistParam.iSubShift = 2;
      }
      else
      {
        m_cDistParam.iSubShift = 1;
      }

      Distortion uiTempSad = m_cDistParam.DistFunc( &m_cDistParam );
      if((uiTempSad + uiBitCost) < rcStruct.uiBestSad)
      {
        uiSad += uiTempSad >>  m_cDistParam.iSubShift;
        while(m_cDistParam.iSubShift > 0)
        {
          isubShift         = m_cDistParam.iSubShift -1;
          m_cDistParam.pOrg = pcPatternKey->getROIY() + (pcPatternKey->getPatternLStride() << isubShift);
          m_cDistParam.pCur = piRefSrch + (rcStruct.iYStride << isubShift);
          uiTempSad = m_cDistParam.DistFunc( &m_cDistParam );
          uiSad += uiTempSad >>  m_cDistParam.iSubShift;
          if(((uiSad << isubShift) + uiBitCost) > rcStruct.uiBestSad)
          {
            break;
          }

          m_cDistParam.iSubShift--;
        }

        if(m_cDistParam.iSubShift == 0)
        {
          uiSad += uiBitCost;
          if( uiSad < rcStruct.uiBestSad )
          {
            rcStruct.uiBestSad      = uiSad;
            rcStruct.iBestX         = iSearchX;
            rcStruct.iBestY         = iSearchY;
            rcStruct.uiBestDistance = uiDistance;
            rcStruct.uiBestRound    = 0;
            rcStruct.ucPointNr      = ucPointNr;
            m_cDistParam.m_maximumDistortionForEarlyExit = uiSad;
          }
        }
      }
    }
  }
  else
  {
    // fast encoder decision: use subsampled SAD when rows > 8 for integer ME
    if ( m_pcEncCfg->getFastInterSearchMode()==FASTINTERSEARCH_MODE1 || m_pcEncCfg->getFastInterSearchMode()==FASTINTERSEARCH_MODE3 )
    {
      if ( m_cDistParam.iRows > 8 )
      {
        m_cDistParam.iSubShift = 1;
      }
    }

    uiSad = m_cDistParam.DistFunc( &m_cDistParam );

    // EMI: If save is true, store the values of SSE in a dynamic array
    if(save) {array_e.push_back(uiSad);}

    // only add motion cost if uiSad is smaller than best. Otherwise pointless
    // to add motion cost.
    if( uiSad < rcStruct.uiBestSad )
    {
      // motion cost
      uiSad += m_pcRdCost->getCostOfVectorWithPredictor( iSearchX, iSearchY );
      if( uiSad < rcStruct.uiBestSad )
      {
        rcStruct.uiBestSad      = uiSad;
        rcStruct.iBestX         = iSearchX;
        rcStruct.iBestY         = iSearchY;
        rcStruct.uiBestDistance = uiDistance;
        rcStruct.uiBestRound    = 0;
        rcStruct.ucPointNr      = ucPointNr;
        m_cDistParam.m_maximumDistortionForEarlyExit = uiSad;
      }
    }
  }
}

__inline Void TEncSearch::xTZ2PointSearch( const TComPattern* const pcPatternKey, IntTZSearchStruct& rcStruct, const TComMv* const pcMvSrchRngLT, const TComMv* const pcMvSrchRngRB )
{
  Int   iSrchRngHorLeft   = pcMvSrchRngLT->getHor();
  Int   iSrchRngHorRight  = pcMvSrchRngRB->getHor();
  Int   iSrchRngVerTop    = pcMvSrchRngLT->getVer();
  Int   iSrchRngVerBottom = pcMvSrchRngRB->getVer();

  // 2 point search,                   //   1 2 3
  // check only the 2 untested points  //   4 0 5
  // around the start point            //   6 7 8
  Int iStartX = rcStruct.iBestX;
  Int iStartY = rcStruct.iBestY;
  switch( rcStruct.ucPointNr )
  {
    case 1:
    {
      if ( (iStartX - 1) >= iSrchRngHorLeft )
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX - 1, iStartY, 0, 2 );
      }
      if ( (iStartY - 1) >= iSrchRngVerTop )
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iStartY - 1, 0, 2 );
      }
    }
      break;
    case 2:
    {
      if ( (iStartY - 1) >= iSrchRngVerTop )
      {
        if ( (iStartX - 1) >= iSrchRngHorLeft )
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX - 1, iStartY - 1, 0, 2 );
        }
        if ( (iStartX + 1) <= iSrchRngHorRight )
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX + 1, iStartY - 1, 0, 2 );
        }
      }
    }
      break;
    case 3:
    {
      if ( (iStartY - 1) >= iSrchRngVerTop )
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iStartY - 1, 0, 2 );
      }
      if ( (iStartX + 1) <= iSrchRngHorRight )
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX + 1, iStartY, 0, 2 );
      }
    }
      break;
    case 4:
    {
      if ( (iStartX - 1) >= iSrchRngHorLeft )
      {
        if ( (iStartY + 1) <= iSrchRngVerBottom )
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX - 1, iStartY + 1, 0, 2 );
        }
        if ( (iStartY - 1) >= iSrchRngVerTop )
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX - 1, iStartY - 1, 0, 2 );
        }
      }
    }
      break;
    case 5:
    {
      if ( (iStartX + 1) <= iSrchRngHorRight )
      {
        if ( (iStartY - 1) >= iSrchRngVerTop )
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX + 1, iStartY - 1, 0, 2 );
        }
        if ( (iStartY + 1) <= iSrchRngVerBottom )
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX + 1, iStartY + 1, 0, 2 );
        }
      }
    }
      break;
    case 6:
    {
      if ( (iStartX - 1) >= iSrchRngHorLeft )
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX - 1, iStartY , 0, 2 );
      }
      if ( (iStartY + 1) <= iSrchRngVerBottom )
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iStartY + 1, 0, 2 );
      }
    }
      break;
    case 7:
    {
      if ( (iStartY + 1) <= iSrchRngVerBottom )
      {
        if ( (iStartX - 1) >= iSrchRngHorLeft )
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX - 1, iStartY + 1, 0, 2 );
        }
        if ( (iStartX + 1) <= iSrchRngHorRight )
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX + 1, iStartY + 1, 0, 2 );
        }
      }
    }
      break;
    case 8:
    {
      if ( (iStartX + 1) <= iSrchRngHorRight )
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX + 1, iStartY, 0, 2 );
      }
      if ( (iStartY + 1) <= iSrchRngVerBottom )
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iStartY + 1, 0, 2 );
      }
    }
      break;
    default:
    {
      assert( false );
    }
      break;
  } // switch( rcStruct.ucPointNr )
}




__inline Void TEncSearch::xTZ8PointSquareSearch( const TComPattern* const pcPatternKey, IntTZSearchStruct& rcStruct, const TComMv* const pcMvSrchRngLT, const TComMv* const pcMvSrchRngRB, const Int iStartX, const Int iStartY, const Int iDist, bool save )
{
  const Int   iSrchRngHorLeft   = pcMvSrchRngLT->getHor();
  const Int   iSrchRngHorRight  = pcMvSrchRngRB->getHor();
  const Int   iSrchRngVerTop    = pcMvSrchRngLT->getVer();
  const Int   iSrchRngVerBottom = pcMvSrchRngRB->getVer();

  // 8 point search,                   //   1 2 3
  // search around the start point     //   4 0 5
  // with the required  distance       //   6 7 8
  assert( iDist != 0 );
  const Int iTop        = iStartY - iDist;
  const Int iBottom     = iStartY + iDist;
  const Int iLeft       = iStartX - iDist;
  const Int iRight      = iStartX + iDist;
  rcStruct.uiBestRound += 1;

  if ( iTop >= iSrchRngVerTop ) // check top
  {
    if ( iLeft >= iSrchRngHorLeft ) // check top left
    {
      xTZSearchHelp( pcPatternKey, rcStruct, iLeft, iTop, 1, iDist, save );
    }
    // top middle
    xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iTop, 2, iDist, save );

    if ( iRight <= iSrchRngHorRight ) // check top right
    {
      xTZSearchHelp( pcPatternKey, rcStruct, iRight, iTop, 3, iDist, save );
    }
  } // check top
  if ( iLeft >= iSrchRngHorLeft ) // check middle left
  {
    xTZSearchHelp( pcPatternKey, rcStruct, iLeft, iStartY, 4, iDist, save );
  }
  if ( iRight <= iSrchRngHorRight ) // check middle right
  {
    xTZSearchHelp( pcPatternKey, rcStruct, iRight, iStartY, 5, iDist, save );
  }
  if ( iBottom <= iSrchRngVerBottom ) // check bottom
  {
    if ( iLeft >= iSrchRngHorLeft ) // check bottom left
    {
      xTZSearchHelp( pcPatternKey, rcStruct, iLeft, iBottom, 6, iDist, save );
    }
    // check bottom middle
    xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iBottom, 7, iDist, save );

    if ( iRight <= iSrchRngHorRight ) // check bottom right
    {
      xTZSearchHelp( pcPatternKey, rcStruct, iRight, iBottom, 8, iDist, save );
    }
  } // check bottom
}



__inline Void TEncSearch::xTZ8PointDiamondSearch( const TComPattern*const  pcPatternKey,
                                                  IntTZSearchStruct& rcStruct,
                                                  const TComMv*const  pcMvSrchRngLT,
                                                  const TComMv*const  pcMvSrchRngRB,
                                                  const Int iStartX,
                                                  const Int iStartY,
                                                  const Int iDist,
                                                  const Bool bCheckCornersAtDist1 )
{
  const Int   iSrchRngHorLeft   = pcMvSrchRngLT->getHor();
  const Int   iSrchRngHorRight  = pcMvSrchRngRB->getHor();
  const Int   iSrchRngVerTop    = pcMvSrchRngLT->getVer();
  const Int   iSrchRngVerBottom = pcMvSrchRngRB->getVer();

  // 8 point search,                   //   1 2 3
  // search around the start point     //   4 0 5
  // with the required  distance       //   6 7 8
  assert ( iDist != 0 );
  const Int iTop        = iStartY - iDist;
  const Int iBottom     = iStartY + iDist;
  const Int iLeft       = iStartX - iDist;
  const Int iRight      = iStartX + iDist;
  rcStruct.uiBestRound += 1;

  if ( iDist == 1 )
  {
    if ( iTop >= iSrchRngVerTop ) // check top
    {
      if (bCheckCornersAtDist1)
      {
        if ( iLeft >= iSrchRngHorLeft) // check top-left
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iLeft, iTop, 1, iDist );
        }
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iTop, 2, iDist );
        if ( iRight <= iSrchRngHorRight ) // check middle right
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iRight, iTop, 3, iDist );
        }
      }
      else
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iTop, 2, iDist );
      }
    }
    if ( iLeft >= iSrchRngHorLeft ) // check middle left
    {
      xTZSearchHelp( pcPatternKey, rcStruct, iLeft, iStartY, 4, iDist );
    }
    if ( iRight <= iSrchRngHorRight ) // check middle right
    {
      xTZSearchHelp( pcPatternKey, rcStruct, iRight, iStartY, 5, iDist );
    }
    if ( iBottom <= iSrchRngVerBottom ) // check bottom
    {
      if (bCheckCornersAtDist1)
      {
        if ( iLeft >= iSrchRngHorLeft) // check top-left
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iLeft, iBottom, 6, iDist );
        }
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iBottom, 7, iDist );
        if ( iRight <= iSrchRngHorRight ) // check middle right
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iRight, iBottom, 8, iDist );
        }
      }
      else
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iBottom, 7, iDist );
      }
    }
  }
  else
  {
    if ( iDist <= 8 )
    {
      const Int iTop_2      = iStartY - (iDist>>1);
      const Int iBottom_2   = iStartY + (iDist>>1);
      const Int iLeft_2     = iStartX - (iDist>>1);
      const Int iRight_2    = iStartX + (iDist>>1);

      if (  iTop >= iSrchRngVerTop && iLeft >= iSrchRngHorLeft &&
          iRight <= iSrchRngHorRight && iBottom <= iSrchRngVerBottom ) // check border
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX,  iTop,      2, iDist    );
        xTZSearchHelp( pcPatternKey, rcStruct, iLeft_2,  iTop_2,    1, iDist>>1 );
        xTZSearchHelp( pcPatternKey, rcStruct, iRight_2, iTop_2,    3, iDist>>1 );
        xTZSearchHelp( pcPatternKey, rcStruct, iLeft,    iStartY,   4, iDist    );
        xTZSearchHelp( pcPatternKey, rcStruct, iRight,   iStartY,   5, iDist    );
        xTZSearchHelp( pcPatternKey, rcStruct, iLeft_2,  iBottom_2, 6, iDist>>1 );
        xTZSearchHelp( pcPatternKey, rcStruct, iRight_2, iBottom_2, 8, iDist>>1 );
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX,  iBottom,   7, iDist    );
      }
      else // check border
      {
        if ( iTop >= iSrchRngVerTop ) // check top
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iTop, 2, iDist );
        }
        if ( iTop_2 >= iSrchRngVerTop ) // check half top
        {
          if ( iLeft_2 >= iSrchRngHorLeft ) // check half left
          {
            xTZSearchHelp( pcPatternKey, rcStruct, iLeft_2, iTop_2, 1, (iDist>>1) );
          }
          if ( iRight_2 <= iSrchRngHorRight ) // check half right
          {
            xTZSearchHelp( pcPatternKey, rcStruct, iRight_2, iTop_2, 3, (iDist>>1) );
          }
        } // check half top
        if ( iLeft >= iSrchRngHorLeft ) // check left
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iLeft, iStartY, 4, iDist );
        }
        if ( iRight <= iSrchRngHorRight ) // check right
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iRight, iStartY, 5, iDist );
        }
        if ( iBottom_2 <= iSrchRngVerBottom ) // check half bottom
        {
          if ( iLeft_2 >= iSrchRngHorLeft ) // check half left
          {
            xTZSearchHelp( pcPatternKey, rcStruct, iLeft_2, iBottom_2, 6, (iDist>>1) );
          }
          if ( iRight_2 <= iSrchRngHorRight ) // check half right
          {
            xTZSearchHelp( pcPatternKey, rcStruct, iRight_2, iBottom_2, 8, (iDist>>1) );
          }
        } // check half bottom
        if ( iBottom <= iSrchRngVerBottom ) // check bottom
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iBottom, 7, iDist );
        }
      } // check border
    }
    else // iDist > 8
    {
      if ( iTop >= iSrchRngVerTop && iLeft >= iSrchRngHorLeft &&
          iRight <= iSrchRngHorRight && iBottom <= iSrchRngVerBottom ) // check border
      {
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iTop,    0, iDist );
        xTZSearchHelp( pcPatternKey, rcStruct, iLeft,   iStartY, 0, iDist );
        xTZSearchHelp( pcPatternKey, rcStruct, iRight,  iStartY, 0, iDist );
        xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iBottom, 0, iDist );
        for ( Int index = 1; index < 4; index++ )
        {
          const Int iPosYT = iTop    + ((iDist>>2) * index);
          const Int iPosYB = iBottom - ((iDist>>2) * index);
          const Int iPosXL = iStartX - ((iDist>>2) * index);
          const Int iPosXR = iStartX + ((iDist>>2) * index);
          xTZSearchHelp( pcPatternKey, rcStruct, iPosXL, iPosYT, 0, iDist );
          xTZSearchHelp( pcPatternKey, rcStruct, iPosXR, iPosYT, 0, iDist );
          xTZSearchHelp( pcPatternKey, rcStruct, iPosXL, iPosYB, 0, iDist );
          xTZSearchHelp( pcPatternKey, rcStruct, iPosXR, iPosYB, 0, iDist );
        }
      }
      else // check border
      {
        if ( iTop >= iSrchRngVerTop ) // check top
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iTop, 0, iDist );
        }
        if ( iLeft >= iSrchRngHorLeft ) // check left
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iLeft, iStartY, 0, iDist );
        }
        if ( iRight <= iSrchRngHorRight ) // check right
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iRight, iStartY, 0, iDist );
        }
        if ( iBottom <= iSrchRngVerBottom ) // check bottom
        {
          xTZSearchHelp( pcPatternKey, rcStruct, iStartX, iBottom, 0, iDist );
        }
        for ( Int index = 1; index < 4; index++ )
        {
          const Int iPosYT = iTop    + ((iDist>>2) * index);
          const Int iPosYB = iBottom - ((iDist>>2) * index);
          const Int iPosXL = iStartX - ((iDist>>2) * index);
          const Int iPosXR = iStartX + ((iDist>>2) * index);

          if ( iPosYT >= iSrchRngVerTop ) // check top
          {
            if ( iPosXL >= iSrchRngHorLeft ) // check left
            {
              xTZSearchHelp( pcPatternKey, rcStruct, iPosXL, iPosYT, 0, iDist );
            }
            if ( iPosXR <= iSrchRngHorRight ) // check right
            {
              xTZSearchHelp( pcPatternKey, rcStruct, iPosXR, iPosYT, 0, iDist );
            }
          } // check top
          if ( iPosYB <= iSrchRngVerBottom ) // check bottom
          {
            if ( iPosXL >= iSrchRngHorLeft ) // check left
            {
              xTZSearchHelp( pcPatternKey, rcStruct, iPosXL, iPosYB, 0, iDist );
            }
            if ( iPosXR <= iSrchRngHorRight ) // check right
            {
              xTZSearchHelp( pcPatternKey, rcStruct, iPosXR, iPosYB, 0, iDist );
            }
          } // check bottom
        } // for ...
      } // check border
    } // iDist <= 8
  } // iDist == 1
}

Distortion TEncSearch::xPatternRefinement( TComPattern* pcPatternKey,
                                           TComMv baseRefMv,
                                           Int iFrac, TComMv& rcMvFrac,
                                           Bool bAllowUseOfHadamard
                                         )
{
  Distortion  uiDist;
  Distortion  uiDistBest  = std::numeric_limits<Distortion>::max();
  UInt        uiDirecBest = 0;

  Pel*  piRefPos;
  Int iRefStride = m_filteredBlock[0][0].getStride(COMPONENT_Y);

  m_pcRdCost->setDistParam( pcPatternKey, m_filteredBlock[0][0].getAddr(COMPONENT_Y), iRefStride, 1, m_cDistParam, m_pcEncCfg->getUseHADME() && bAllowUseOfHadamard );

  const TComMv* pcMvRefine = (iFrac == 2 ? s_acMvRefineH : s_acMvRefineQ);

  for (UInt i = 0; i < 9; i++)
  {
    TComMv cMvTest = pcMvRefine[i];
    cMvTest += baseRefMv;

    Int horVal = cMvTest.getHor() * iFrac;
    Int verVal = cMvTest.getVer() * iFrac;
    piRefPos = m_filteredBlock[ verVal & 3 ][ horVal & 3 ].getAddr(COMPONENT_Y);
    if ( horVal == 2 && ( verVal & 1 ) == 0 )
    {
      piRefPos += 1;
    }
    if ( ( horVal & 1 ) == 0 && verVal == 2 )
    {
      piRefPos += iRefStride;
    }
    cMvTest = pcMvRefine[i];
    cMvTest += rcMvFrac;

    setDistParamComp(COMPONENT_Y);

    m_cDistParam.pCur = piRefPos;
    m_cDistParam.bitDepth = pcPatternKey->getBitDepthY();
    uiDist = m_cDistParam.DistFunc( &m_cDistParam );
    uiDist += m_pcRdCost->getCostOfVectorWithPredictor( cMvTest.getHor(), cMvTest.getVer() );

    if ( uiDist < uiDistBest )
    {
      uiDistBest  = uiDist;
      uiDirecBest = i;
      m_cDistParam.m_maximumDistortionForEarlyExit = uiDist;
    }
  }

  rcMvFrac = pcMvRefine[uiDirecBest];

  return uiDistBest;
}



Void
TEncSearch::xEncSubdivCbfQT(TComTU      &rTu,
                            Bool         bLuma,
                            Bool         bChroma )
{
  TComDataCU* pcCU=rTu.getCU();
  const UInt uiAbsPartIdx         = rTu.GetAbsPartIdxTU();
  const UInt uiTrDepth            = rTu.GetTransformDepthRel();
  const UInt uiTrMode             = pcCU->getTransformIdx( uiAbsPartIdx );
  const UInt uiSubdiv             = ( uiTrMode > uiTrDepth ? 1 : 0 );
  const UInt uiLog2LumaTrafoSize  = rTu.GetLog2LumaTrSize();

  if( pcCU->isIntra(0) && pcCU->getPartitionSize(0) == SIZE_NxN && uiTrDepth == 0 )
  {
    assert( uiSubdiv );
  }
  else if( uiLog2LumaTrafoSize > pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() )
  {
    assert( uiSubdiv );
  }
  else if( uiLog2LumaTrafoSize == pcCU->getSlice()->getSPS()->getQuadtreeTULog2MinSize() )
  {
    assert( !uiSubdiv );
  }
  else if( uiLog2LumaTrafoSize == pcCU->getQuadtreeTULog2MinSizeInCU(uiAbsPartIdx) )
  {
    assert( !uiSubdiv );
  }
  else
  {
    assert( uiLog2LumaTrafoSize > pcCU->getQuadtreeTULog2MinSizeInCU(uiAbsPartIdx) );
    if( bLuma )
    {
      m_pcEntropyCoder->encodeTransformSubdivFlag( uiSubdiv, 5 - uiLog2LumaTrafoSize );
    }
  }

  if ( bChroma )
  {
    const UInt numberValidComponents = getNumberValidComponents(rTu.GetChromaFormat());
    for (UInt ch=COMPONENT_Cb; ch<numberValidComponents; ch++)
    {
      const ComponentID compID=ComponentID(ch);
      if( rTu.ProcessingAllQuadrants(compID) && (uiTrDepth==0 || pcCU->getCbf( uiAbsPartIdx, compID, uiTrDepth-1 ) ))
      {
        m_pcEntropyCoder->encodeQtCbf(rTu, compID, (uiSubdiv == 0));
      }
    }
  }

  if( uiSubdiv )
  {
    TComTURecurse tuRecurse(rTu, false);
    do
    {
      xEncSubdivCbfQT( tuRecurse, bLuma, bChroma );
    } while (tuRecurse.nextSection(rTu));
  }
  else
  {
    //===== Cbfs =====
    if( bLuma )
    {
      m_pcEntropyCoder->encodeQtCbf( rTu, COMPONENT_Y, true );
    }
  }
}




Void
TEncSearch::xEncCoeffQT(TComTU &rTu,
                        const ComponentID  component,
                        Bool         bRealCoeff )
{
  TComDataCU* pcCU=rTu.getCU();
  const UInt uiAbsPartIdx = rTu.GetAbsPartIdxTU();
  const UInt uiTrDepth=rTu.GetTransformDepthRel();

  const UInt  uiTrMode        = pcCU->getTransformIdx( uiAbsPartIdx );
  const UInt  uiSubdiv        = ( uiTrMode > uiTrDepth ? 1 : 0 );

  if( uiSubdiv )
  {
    TComTURecurse tuRecurseChild(rTu, false);
    do
    {
      xEncCoeffQT( tuRecurseChild, component, bRealCoeff );
    } while (tuRecurseChild.nextSection(rTu) );
  }
  else if (rTu.ProcessComponentSection(component))
  {
    //===== coefficients =====
    const UInt  uiLog2TrafoSize = rTu.GetLog2LumaTrSize();
    UInt    uiCoeffOffset   = rTu.getCoefficientOffset(component);
    UInt    uiQTLayer       = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() - uiLog2TrafoSize;
    TCoeff* pcCoeff         = bRealCoeff ? pcCU->getCoeff(component) : m_ppcQTTempCoeff[component][uiQTLayer];

    if (isChroma(component) && (pcCU->getCbf( rTu.GetAbsPartIdxTU(), COMPONENT_Y, uiTrMode ) != 0) && pcCU->getSlice()->getPPS()->getPpsRangeExtension().getCrossComponentPredictionEnabledFlag() )
    {
      m_pcEntropyCoder->encodeCrossComponentPrediction( rTu, component );
    }

    m_pcEntropyCoder->encodeCoeffNxN( rTu, pcCoeff+uiCoeffOffset, component );
  }
}




Void
TEncSearch::xEncIntraHeader( TComDataCU*  pcCU,
                            UInt         uiTrDepth,
                            UInt         uiAbsPartIdx,
                            Bool         bLuma,
                            Bool         bChroma )
{
  if( bLuma )
  {
    // CU header
    if( uiAbsPartIdx == 0 )
    {
      if( !pcCU->getSlice()->isIntra() )
      {
        if (pcCU->getSlice()->getPPS()->getTransquantBypassEnableFlag())
        {
          m_pcEntropyCoder->encodeCUTransquantBypassFlag( pcCU, 0, true );
        }
        m_pcEntropyCoder->encodeSkipFlag( pcCU, 0, true );
        m_pcEntropyCoder->encodePredMode( pcCU, 0, true );
      }
      m_pcEntropyCoder  ->encodePartSize( pcCU, 0, pcCU->getDepth(0), true );

      if (pcCU->isIntra(0) && pcCU->getPartitionSize(0) == SIZE_2Nx2N )
      {
        m_pcEntropyCoder->encodeIPCMInfo( pcCU, 0, true );

        if ( pcCU->getIPCMFlag (0))
        {
          return;
        }
      }
    }
    // luma prediction mode
    if( pcCU->getPartitionSize(0) == SIZE_2Nx2N )
    {
      if (uiAbsPartIdx==0)
      {
        m_pcEntropyCoder->encodeIntraDirModeLuma ( pcCU, 0 );
      }
    }
    else
    {
      UInt uiQNumParts = pcCU->getTotalNumPart() >> 2;
      if (uiTrDepth>0 && (uiAbsPartIdx%uiQNumParts)==0)
      {
        m_pcEntropyCoder->encodeIntraDirModeLuma ( pcCU, uiAbsPartIdx );
      }
    }
  }

  if( bChroma )
  {
    if( pcCU->getPartitionSize(0) == SIZE_2Nx2N || !enable4ChromaPUsInIntraNxNCU(pcCU->getPic()->getChromaFormat()))
    {
      if(uiAbsPartIdx==0)
      {
         m_pcEntropyCoder->encodeIntraDirModeChroma ( pcCU, uiAbsPartIdx );
      }
    }
    else
    {
      UInt uiQNumParts = pcCU->getTotalNumPart() >> 2;
      assert(uiTrDepth>0);
      if ((uiAbsPartIdx%uiQNumParts)==0)
      {
        m_pcEntropyCoder->encodeIntraDirModeChroma ( pcCU, uiAbsPartIdx );
      }
    }
  }
}




UInt
TEncSearch::xGetIntraBitsQT(TComTU &rTu,
                            Bool         bLuma,
                            Bool         bChroma,
                            Bool         bRealCoeff /* just for test */ )
{
  TComDataCU* pcCU=rTu.getCU();
  const UInt uiAbsPartIdx = rTu.GetAbsPartIdxTU();
  const UInt uiTrDepth=rTu.GetTransformDepthRel();
  m_pcEntropyCoder->resetBits();
  xEncIntraHeader ( pcCU, uiTrDepth, uiAbsPartIdx, bLuma, bChroma );
  xEncSubdivCbfQT ( rTu, bLuma, bChroma );

  if( bLuma )
  {
    xEncCoeffQT   ( rTu, COMPONENT_Y,      bRealCoeff );
  }
  if( bChroma )
  {
    xEncCoeffQT   ( rTu, COMPONENT_Cb,  bRealCoeff );
    xEncCoeffQT   ( rTu, COMPONENT_Cr,  bRealCoeff );
  }
  UInt   uiBits = m_pcEntropyCoder->getNumberOfWrittenBits();

  return uiBits;
}

UInt TEncSearch::xGetIntraBitsQTChroma(TComTU &rTu,
                                       ComponentID compID,
                                       Bool         bRealCoeff /* just for test */ )
{
  m_pcEntropyCoder->resetBits();
  xEncCoeffQT   ( rTu, compID,  bRealCoeff );
  UInt   uiBits = m_pcEntropyCoder->getNumberOfWrittenBits();
  return uiBits;
}

Void TEncSearch::xIntraCodingTUBlock(       TComYuv*    pcOrgYuv,
                                            TComYuv*    pcPredYuv,
                                            TComYuv*    pcResiYuv,
                                            Pel         resiLuma[NUMBER_OF_STORED_RESIDUAL_TYPES][MAX_CU_SIZE * MAX_CU_SIZE],
                                      const Bool        checkCrossCPrediction,
                                            Distortion& ruiDist,
                                      const ComponentID compID,
                                            TComTU&     rTu
                                      DEBUG_STRING_FN_DECLARE(sDebug)
                                           ,Int         default0Save1Load2
                                     )
{
  if (!rTu.ProcessComponentSection(compID))
  {
    return;
  }
  const Bool           bIsLuma          = isLuma(compID);
  const TComRectangle &rect             = rTu.getRect(compID);
        TComDataCU    *pcCU             = rTu.getCU();
  const UInt           uiAbsPartIdx     = rTu.GetAbsPartIdxTU();
  const TComSPS       &sps              = *(pcCU->getSlice()->getSPS());

  const UInt           uiTrDepth        = rTu.GetTransformDepthRelAdj(compID);
  const UInt           uiFullDepth      = rTu.GetTransformDepthTotal();
  const UInt           uiLog2TrSize     = rTu.GetLog2LumaTrSize();
  const ChromaFormat   chFmt            = pcOrgYuv->getChromaFormat();
  const ChannelType    chType           = toChannelType(compID);
  const Int            bitDepth         = sps.getBitDepth(chType);

  const UInt           uiWidth          = rect.width;
  const UInt           uiHeight         = rect.height;
  const UInt           uiStride         = pcOrgYuv ->getStride (compID);
        Pel           *piOrg            = pcOrgYuv ->getAddr( compID, uiAbsPartIdx );
        Pel           *piPred           = pcPredYuv->getAddr( compID, uiAbsPartIdx );
        Pel           *piResi           = pcResiYuv->getAddr( compID, uiAbsPartIdx );
        Pel           *piReco           = pcPredYuv->getAddr( compID, uiAbsPartIdx );
  const UInt           uiQTLayer        = sps.getQuadtreeTULog2MaxSize() - uiLog2TrSize;
        Pel           *piRecQt          = m_pcQTTempTComYuv[ uiQTLayer ].getAddr( compID, uiAbsPartIdx );
  const UInt           uiRecQtStride    = m_pcQTTempTComYuv[ uiQTLayer ].getStride(compID);
  const UInt           uiZOrder         = pcCU->getZorderIdxInCtu() + uiAbsPartIdx;
        Pel           *piRecIPred       = pcCU->getPic()->getPicYuvRec()->getAddr( compID, pcCU->getCtuRsAddr(), uiZOrder );
        UInt           uiRecIPredStride = pcCU->getPic()->getPicYuvRec()->getStride  ( compID );
        TCoeff        *pcCoeff          = m_ppcQTTempCoeff[compID][uiQTLayer] + rTu.getCoefficientOffset(compID);
        Bool           useTransformSkip = pcCU->getTransformSkip(uiAbsPartIdx, compID);

#if ADAPTIVE_QP_SELECTION
        TCoeff        *pcArlCoeff       = m_ppcQTTempArlCoeff[compID][ uiQTLayer ] + rTu.getCoefficientOffset(compID);
#endif

  const UInt           uiChPredMode     = pcCU->getIntraDir( chType, uiAbsPartIdx );
  const UInt           partsPerMinCU    = 1<<(2*(sps.getMaxTotalCUDepth() - sps.getLog2DiffMaxMinCodingBlockSize()));
  const UInt           uiChCodedMode    = (uiChPredMode==DM_CHROMA_IDX && !bIsLuma) ? pcCU->getIntraDir(CHANNEL_TYPE_LUMA, getChromasCorrespondingPULumaIdx(uiAbsPartIdx, chFmt, partsPerMinCU)) : uiChPredMode;
  const UInt           uiChFinalMode    = ((chFmt == CHROMA_422)       && !bIsLuma) ? g_chroma422IntraAngleMappingTable[uiChCodedMode] : uiChCodedMode;

  const Int            blkX                                 = g_auiRasterToPelX[ g_auiZscanToRaster[ uiAbsPartIdx ] ];
  const Int            blkY                                 = g_auiRasterToPelY[ g_auiZscanToRaster[ uiAbsPartIdx ] ];
  const Int            bufferOffset                         = blkX + (blkY * MAX_CU_SIZE);
        Pel  *const    encoderLumaResidual                  = resiLuma[RESIDUAL_ENCODER_SIDE ] + bufferOffset;
        Pel  *const    reconstructedLumaResidual            = resiLuma[RESIDUAL_RECONSTRUCTED] + bufferOffset;
  const Bool           bUseCrossCPrediction                 = isChroma(compID) && (uiChPredMode == DM_CHROMA_IDX) && checkCrossCPrediction;
  const Bool           bUseReconstructedResidualForEstimate = m_pcEncCfg->getUseReconBasedCrossCPredictionEstimate();
        Pel *const     lumaResidualForEstimate              = bUseReconstructedResidualForEstimate ? reconstructedLumaResidual : encoderLumaResidual;

#if DEBUG_STRING
  const Int debugPredModeMask=DebugStringGetPredModeMask(MODE_INTRA);
#endif

  //===== init availability pattern =====
  DEBUG_STRING_NEW(sTemp)

#if !DEBUG_STRING
  if( default0Save1Load2 != 2 )
#endif
  {
    const Bool bUseFilteredPredictions=TComPrediction::filteringIntraReferenceSamples(compID, uiChFinalMode, uiWidth, uiHeight, chFmt, sps.getSpsRangeExtension().getIntraSmoothingDisabledFlag());

    initIntraPatternChType( rTu, compID, bUseFilteredPredictions DEBUG_STRING_PASS_INTO(sDebug) );

    //===== get prediction signal =====
    predIntraAng( compID, uiChFinalMode, piOrg, uiStride, piPred, uiStride, rTu, bUseFilteredPredictions );

    // save prediction
    if( default0Save1Load2 == 1 )
    {
      Pel*  pPred   = piPred;
      Pel*  pPredBuf = m_pSharedPredTransformSkip[compID];
      Int k = 0;
      for( UInt uiY = 0; uiY < uiHeight; uiY++ )
      {
        for( UInt uiX = 0; uiX < uiWidth; uiX++ )
        {
          pPredBuf[ k ++ ] = pPred[ uiX ];
        }
        pPred += uiStride;
      }
    }
  }
#if !DEBUG_STRING
  else
  {
    // load prediction
    Pel*  pPred   = piPred;
    Pel*  pPredBuf = m_pSharedPredTransformSkip[compID];
    Int k = 0;
    for( UInt uiY = 0; uiY < uiHeight; uiY++ )
    {
      for( UInt uiX = 0; uiX < uiWidth; uiX++ )
      {
        pPred[ uiX ] = pPredBuf[ k ++ ];
      }
      pPred += uiStride;
    }
  }
#endif

  //===== get residual signal =====
  {
    // get residual
    Pel*  pOrg    = piOrg;
    Pel*  pPred   = piPred;
    Pel*  pResi   = piResi;

    for( UInt uiY = 0; uiY < uiHeight; uiY++ )
    {
      for( UInt uiX = 0; uiX < uiWidth; uiX++ )
      {
        pResi[ uiX ] = pOrg[ uiX ] - pPred[ uiX ];
      }

      pOrg  += uiStride;
      pResi += uiStride;
      pPred += uiStride;
    }
  }

  if (pcCU->getSlice()->getPPS()->getPpsRangeExtension().getCrossComponentPredictionEnabledFlag())
  {
    if (bUseCrossCPrediction)
    {
      if (xCalcCrossComponentPredictionAlpha( rTu, compID, lumaResidualForEstimate, piResi, uiWidth, uiHeight, MAX_CU_SIZE, uiStride ) == 0)
      {
        return;
      }
      TComTrQuant::crossComponentPrediction ( rTu, compID, reconstructedLumaResidual, piResi, piResi, uiWidth, uiHeight, MAX_CU_SIZE, uiStride, uiStride, false );
    }
    else if (isLuma(compID) && !bUseReconstructedResidualForEstimate)
    {
      xStoreCrossComponentPredictionResult( encoderLumaResidual, piResi, rTu, 0, 0, MAX_CU_SIZE, uiStride );
    }
  }

  //===== transform and quantization =====
  //--- init rate estimation arrays for RDOQ ---
  if( useTransformSkip ? m_pcEncCfg->getUseRDOQTS() : m_pcEncCfg->getUseRDOQ() )
  {
    m_pcEntropyCoder->estimateBit( m_pcTrQuant->m_pcEstBitsSbac, uiWidth, uiHeight, chType );
  }

  //--- transform and quantization ---
  TCoeff uiAbsSum = 0;
  if (bIsLuma)
  {
    pcCU       ->setTrIdxSubParts ( uiTrDepth, uiAbsPartIdx, uiFullDepth );
  }

  const QpParam cQP(*pcCU, compID);

#if RDOQ_CHROMA_LAMBDA
  m_pcTrQuant->selectLambda     (compID);
#endif

  m_pcTrQuant->transformNxN     ( rTu, compID, piResi, uiStride, pcCoeff,
#if ADAPTIVE_QP_SELECTION
    pcArlCoeff,
#endif
    uiAbsSum, cQP
    );

  //--- inverse transform ---

#if DEBUG_STRING
  if ( (uiAbsSum > 0) || (DebugOptionList::DebugString_InvTran.getInt()&debugPredModeMask) )
#else
  if ( uiAbsSum > 0 )
#endif
  {
    m_pcTrQuant->invTransformNxN ( rTu, compID, piResi, uiStride, pcCoeff, cQP DEBUG_STRING_PASS_INTO_OPTIONAL(&sDebug, (DebugOptionList::DebugString_InvTran.getInt()&debugPredModeMask)) );
  }
  else
  {
    Pel* pResi = piResi;
    memset( pcCoeff, 0, sizeof( TCoeff ) * uiWidth * uiHeight );
    for( UInt uiY = 0; uiY < uiHeight; uiY++ )
    {
      memset( pResi, 0, sizeof( Pel ) * uiWidth );
      pResi += uiStride;
    }
  }


  //===== reconstruction =====
  {
    Pel* pPred      = piPred;
    Pel* pResi      = piResi;
    Pel* pReco      = piReco;
    Pel* pRecQt     = piRecQt;
    Pel* pRecIPred  = piRecIPred;

    if (pcCU->getSlice()->getPPS()->getPpsRangeExtension().getCrossComponentPredictionEnabledFlag())
    {
      if (bUseCrossCPrediction)
      {
        TComTrQuant::crossComponentPrediction( rTu, compID, reconstructedLumaResidual, piResi, piResi, uiWidth, uiHeight, MAX_CU_SIZE, uiStride, uiStride, true );
      }
      else if (isLuma(compID))
      {
        xStoreCrossComponentPredictionResult( reconstructedLumaResidual, piResi, rTu, 0, 0, MAX_CU_SIZE, uiStride );
      }
    }

 #if DEBUG_STRING
    std::stringstream ss(stringstream::out);
    const Bool bDebugPred=((DebugOptionList::DebugString_Pred.getInt()&debugPredModeMask) && DEBUG_STRING_CHANNEL_CONDITION(compID));
    const Bool bDebugResi=((DebugOptionList::DebugString_Resi.getInt()&debugPredModeMask) && DEBUG_STRING_CHANNEL_CONDITION(compID));
    const Bool bDebugReco=((DebugOptionList::DebugString_Reco.getInt()&debugPredModeMask) && DEBUG_STRING_CHANNEL_CONDITION(compID));

    if (bDebugPred || bDebugResi || bDebugReco)
    {
      ss << "###: " << "CompID: " << compID << " pred mode (ch/fin): " << uiChPredMode << "/" << uiChFinalMode << " absPartIdx: " << rTu.GetAbsPartIdxTU() << "\n";
      for( UInt uiY = 0; uiY < uiHeight; uiY++ )
      {
        ss << "###: ";
        if (bDebugPred)
        {
          ss << " - pred: ";
          for( UInt uiX = 0; uiX < uiWidth; uiX++ )
          {
            ss << pPred[ uiX ] << ", ";
          }
        }
        if (bDebugResi)
        {
          ss << " - resi: ";
        }
        for( UInt uiX = 0; uiX < uiWidth; uiX++ )
        {
          if (bDebugResi)
          {
            ss << pResi[ uiX ] << ", ";
          }
          pReco    [ uiX ] = Pel(ClipBD<Int>( Int(pPred[uiX]) + Int(pResi[uiX]), bitDepth ));
          pRecQt   [ uiX ] = pReco[ uiX ];
          pRecIPred[ uiX ] = pReco[ uiX ];
        }
        if (bDebugReco)
        {
          ss << " - reco: ";
          for( UInt uiX = 0; uiX < uiWidth; uiX++ )
          {
            ss << pReco[ uiX ] << ", ";
          }
        }
        pPred     += uiStride;
        pResi     += uiStride;
        pReco     += uiStride;
        pRecQt    += uiRecQtStride;
        pRecIPred += uiRecIPredStride;
        ss << "\n";
      }
      DEBUG_STRING_APPEND(sDebug, ss.str())
    }
    else
#endif
    {

      for( UInt uiY = 0; uiY < uiHeight; uiY++ )
      {
        for( UInt uiX = 0; uiX < uiWidth; uiX++ )
        {
          pReco    [ uiX ] = Pel(ClipBD<Int>( Int(pPred[uiX]) + Int(pResi[uiX]), bitDepth ));
          pRecQt   [ uiX ] = pReco[ uiX ];
          pRecIPred[ uiX ] = pReco[ uiX ];
        }
        pPred     += uiStride;
        pResi     += uiStride;
        pReco     += uiStride;
        pRecQt    += uiRecQtStride;
        pRecIPred += uiRecIPredStride;
      }
    }
  }

  //===== update distortion =====
  ruiDist += m_pcRdCost->getDistPart( bitDepth, piReco, uiStride, piOrg, uiStride, uiWidth, uiHeight, compID );
}




Void
TEncSearch::xRecurIntraCodingLumaQT(TComYuv*    pcOrgYuv,
                                    TComYuv*    pcPredYuv,
                                    TComYuv*    pcResiYuv,
                                    Pel         resiLuma[NUMBER_OF_STORED_RESIDUAL_TYPES][MAX_CU_SIZE * MAX_CU_SIZE],
                                    Distortion& ruiDistY,
#if HHI_RQT_INTRA_SPEEDUP
                                    Bool        bCheckFirst,
#endif
                                    Double&     dRDCost,
                                    TComTU&     rTu
                                    DEBUG_STRING_FN_DECLARE(sDebug))
{
  TComDataCU   *pcCU          = rTu.getCU();
  const UInt    uiAbsPartIdx  = rTu.GetAbsPartIdxTU();
  const UInt    uiFullDepth   = rTu.GetTransformDepthTotal();
  const UInt    uiTrDepth     = rTu.GetTransformDepthRel();
  const UInt    uiLog2TrSize  = rTu.GetLog2LumaTrSize();
        Bool    bCheckFull    = ( uiLog2TrSize  <= pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() );
        Bool    bCheckSplit   = ( uiLog2TrSize  >  pcCU->getQuadtreeTULog2MinSizeInCU(uiAbsPartIdx) );

        Pel     resiLumaSplit [NUMBER_OF_STORED_RESIDUAL_TYPES][MAX_CU_SIZE * MAX_CU_SIZE];
        Pel     resiLumaSingle[NUMBER_OF_STORED_RESIDUAL_TYPES][MAX_CU_SIZE * MAX_CU_SIZE];

        Bool    bMaintainResidual[NUMBER_OF_STORED_RESIDUAL_TYPES];
        for (UInt residualTypeIndex = 0; residualTypeIndex < NUMBER_OF_STORED_RESIDUAL_TYPES; residualTypeIndex++)
        {
          bMaintainResidual[residualTypeIndex] = true; //assume true unless specified otherwise
        }

        bMaintainResidual[RESIDUAL_ENCODER_SIDE] = !(m_pcEncCfg->getUseReconBasedCrossCPredictionEstimate());

#if HHI_RQT_INTRA_SPEEDUP
  Int maxTuSize = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize();
  Int isIntraSlice = (pcCU->getSlice()->getSliceType() == I_SLICE);
  // don't check split if TU size is less or equal to max TU size
  Bool noSplitIntraMaxTuSize = bCheckFull;
  if(m_pcEncCfg->getRDpenalty() && ! isIntraSlice)
  {
    // in addition don't check split if TU size is less or equal to 16x16 TU size for non-intra slice
    noSplitIntraMaxTuSize = ( uiLog2TrSize  <= min(maxTuSize,4) );

    // if maximum RD-penalty don't check TU size 32x32
    if(m_pcEncCfg->getRDpenalty()==2)
    {
      bCheckFull    = ( uiLog2TrSize  <= min(maxTuSize,4));
    }
  }
  if( bCheckFirst && noSplitIntraMaxTuSize )

  {
    bCheckSplit = false;
  }
#else
  Int maxTuSize = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize();
  Int isIntraSlice = (pcCU->getSlice()->getSliceType() == I_SLICE);
  // if maximum RD-penalty don't check TU size 32x32
  if((m_pcEncCfg->getRDpenalty()==2)  && !isIntraSlice)
  {
    bCheckFull    = ( uiLog2TrSize  <= min(maxTuSize,4));
  }
#endif
  Double     dSingleCost                        = MAX_DOUBLE;
  Distortion uiSingleDistLuma                   = 0;
  UInt       uiSingleCbfLuma                    = 0;
  Bool       checkTransformSkip  = pcCU->getSlice()->getPPS()->getUseTransformSkip();
  Int        bestModeId[MAX_NUM_COMPONENT] = { 0, 0, 0};
  checkTransformSkip           &= TUCompRectHasAssociatedTransformSkipFlag(rTu.getRect(COMPONENT_Y), pcCU->getSlice()->getPPS()->getPpsRangeExtension().getLog2MaxTransformSkipBlockSize());
  checkTransformSkip           &= (!pcCU->getCUTransquantBypass(0));

  assert (rTu.ProcessComponentSection(COMPONENT_Y));
  const UInt totalAdjustedDepthChan   = rTu.GetTransformDepthTotalAdj(COMPONENT_Y);

  if ( m_pcEncCfg->getUseTransformSkipFast() )
  {
    checkTransformSkip       &= (pcCU->getPartitionSize(uiAbsPartIdx)==SIZE_NxN);
  }

  if( bCheckFull )
  {
    if(checkTransformSkip == true)
    {
      //----- store original entropy coding status -----
      m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_ROOT ] );

      Distortion singleDistTmpLuma                    = 0;
      UInt       singleCbfTmpLuma                     = 0;
      Double     singleCostTmp                        = 0;
      Int        firstCheckId                         = 0;

      for(Int modeId = firstCheckId; modeId < 2; modeId ++)
      {
        DEBUG_STRING_NEW(sModeString)
        Int  default0Save1Load2 = 0;
        singleDistTmpLuma=0;
        if(modeId == firstCheckId)
        {
          default0Save1Load2 = 1;
        }
        else
        {
          default0Save1Load2 = 2;
        }


        pcCU->setTransformSkipSubParts ( modeId, COMPONENT_Y, uiAbsPartIdx, totalAdjustedDepthChan );
        xIntraCodingTUBlock( pcOrgYuv, pcPredYuv, pcResiYuv, resiLumaSingle, false, singleDistTmpLuma, COMPONENT_Y, rTu DEBUG_STRING_PASS_INTO(sModeString), default0Save1Load2 );

        singleCbfTmpLuma = pcCU->getCbf( uiAbsPartIdx, COMPONENT_Y, uiTrDepth );

        //----- determine rate and r-d cost -----
        if(modeId == 1 && singleCbfTmpLuma == 0)
        {
          //In order not to code TS flag when cbf is zero, the case for TS with cbf being zero is forbidden.
          singleCostTmp = MAX_DOUBLE;
        }
        else
        {
          UInt uiSingleBits = xGetIntraBitsQT( rTu, true, false, false );
          singleCostTmp     = m_pcRdCost->calcRdCost( uiSingleBits, singleDistTmpLuma );
        }
        if(singleCostTmp < dSingleCost)
        {
          DEBUG_STRING_SWAP(sDebug, sModeString)
          dSingleCost   = singleCostTmp;
          uiSingleDistLuma = singleDistTmpLuma;
          uiSingleCbfLuma = singleCbfTmpLuma;

          bestModeId[COMPONENT_Y] = modeId;
          if(bestModeId[COMPONENT_Y] == firstCheckId)
          {
            xStoreIntraResultQT(COMPONENT_Y, rTu );
            m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ uiFullDepth ][ CI_TEMP_BEST ] );
          }

          if (pcCU->getSlice()->getPPS()->getPpsRangeExtension().getCrossComponentPredictionEnabledFlag())
          {
            const Int xOffset = rTu.getRect( COMPONENT_Y ).x0;
            const Int yOffset = rTu.getRect( COMPONENT_Y ).y0;
            for (UInt storedResidualIndex = 0; storedResidualIndex < NUMBER_OF_STORED_RESIDUAL_TYPES; storedResidualIndex++)
            {
              if (bMaintainResidual[storedResidualIndex])
              {
                xStoreCrossComponentPredictionResult(resiLuma[storedResidualIndex], resiLumaSingle[storedResidualIndex], rTu, xOffset, yOffset, MAX_CU_SIZE, MAX_CU_SIZE);
              }
            }
          }
        }
        if (modeId == firstCheckId)
        {
          m_pcRDGoOnSbacCoder->load ( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_ROOT ] );
        }
      }

      pcCU ->setTransformSkipSubParts ( bestModeId[COMPONENT_Y], COMPONENT_Y, uiAbsPartIdx, totalAdjustedDepthChan );

      if(bestModeId[COMPONENT_Y] == firstCheckId)
      {
        xLoadIntraResultQT(COMPONENT_Y, rTu );
        pcCU->setCbfSubParts  ( uiSingleCbfLuma << uiTrDepth, COMPONENT_Y, uiAbsPartIdx, rTu.GetTransformDepthTotalAdj(COMPONENT_Y) );

        m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[ uiFullDepth ][ CI_TEMP_BEST ] );
      }
    }
    else
    {
      //----- store original entropy coding status -----
      if( bCheckSplit )
      {
        m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_ROOT ] );
      }
      //----- code luma/chroma block with given intra prediction mode and store Cbf-----
      dSingleCost   = 0.0;

      pcCU ->setTransformSkipSubParts ( 0, COMPONENT_Y, uiAbsPartIdx, totalAdjustedDepthChan );
      xIntraCodingTUBlock( pcOrgYuv, pcPredYuv, pcResiYuv, resiLumaSingle, false, uiSingleDistLuma, COMPONENT_Y, rTu DEBUG_STRING_PASS_INTO(sDebug));

      if( bCheckSplit )
      {
        uiSingleCbfLuma = pcCU->getCbf( uiAbsPartIdx, COMPONENT_Y, uiTrDepth );
      }
      //----- determine rate and r-d cost -----
      UInt uiSingleBits = xGetIntraBitsQT( rTu, true, false, false );

      if(m_pcEncCfg->getRDpenalty() && (uiLog2TrSize==5) && !isIntraSlice)
      {
        uiSingleBits=uiSingleBits*4;
      }

      dSingleCost       = m_pcRdCost->calcRdCost( uiSingleBits, uiSingleDistLuma );

      if (pcCU->getSlice()->getPPS()->getPpsRangeExtension().getCrossComponentPredictionEnabledFlag())
      {
        const Int xOffset = rTu.getRect( COMPONENT_Y ).x0;
        const Int yOffset = rTu.getRect( COMPONENT_Y ).y0;
        for (UInt storedResidualIndex = 0; storedResidualIndex < NUMBER_OF_STORED_RESIDUAL_TYPES; storedResidualIndex++)
        {
          if (bMaintainResidual[storedResidualIndex])
          {
            xStoreCrossComponentPredictionResult(resiLuma[storedResidualIndex], resiLumaSingle[storedResidualIndex], rTu, xOffset, yOffset, MAX_CU_SIZE, MAX_CU_SIZE);
          }
        }
      }
    }
  }

  if( bCheckSplit )
  {
    //----- store full entropy coding status, load original entropy coding status -----
    if( bCheckFull )
    {
      m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_TEST ] );
      m_pcRDGoOnSbacCoder->load ( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_ROOT ] );
    }
    else
    {
      m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_ROOT ] );
    }
    //----- code splitted block -----
    Double     dSplitCost      = 0.0;
    Distortion uiSplitDistLuma = 0;
    UInt       uiSplitCbfLuma  = 0;

    TComTURecurse tuRecurseChild(rTu, false);
    DEBUG_STRING_NEW(sSplit)
    do
    {
      DEBUG_STRING_NEW(sChild)
#if HHI_RQT_INTRA_SPEEDUP
      xRecurIntraCodingLumaQT( pcOrgYuv, pcPredYuv, pcResiYuv, resiLumaSplit, uiSplitDistLuma, bCheckFirst, dSplitCost, tuRecurseChild DEBUG_STRING_PASS_INTO(sChild) );
#else
      xRecurIntraCodingLumaQT( pcOrgYuv, pcPredYuv, pcResiYuv, resiLumaSplit, uiSplitDistLuma, dSplitCost, tuRecurseChild DEBUG_STRING_PASS_INTO(sChild) );
#endif
      DEBUG_STRING_APPEND(sSplit, sChild)
      uiSplitCbfLuma |= pcCU->getCbf( tuRecurseChild.GetAbsPartIdxTU(), COMPONENT_Y, tuRecurseChild.GetTransformDepthRel() );
    } while (tuRecurseChild.nextSection(rTu) );

    UInt    uiPartsDiv     = rTu.GetAbsPartIdxNumParts();
    {
      if (uiSplitCbfLuma)
      {
        const UInt flag=1<<uiTrDepth;
        UChar *pBase=pcCU->getCbf( COMPONENT_Y );
        for( UInt uiOffs = 0; uiOffs < uiPartsDiv; uiOffs++ )
        {
          pBase[ uiAbsPartIdx + uiOffs ] |= flag;
        }
      }
    }
    //----- restore context states -----
    m_pcRDGoOnSbacCoder->load ( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_ROOT ] );
    
    //----- determine rate and r-d cost -----
    UInt uiSplitBits = xGetIntraBitsQT( rTu, true, false, false );
    dSplitCost       = m_pcRdCost->calcRdCost( uiSplitBits, uiSplitDistLuma );

    //===== compare and set best =====
    if( dSplitCost < dSingleCost )
    {
      //--- update cost ---
      DEBUG_STRING_SWAP(sSplit, sDebug)
      ruiDistY += uiSplitDistLuma;
      dRDCost  += dSplitCost;

      if (pcCU->getSlice()->getPPS()->getPpsRangeExtension().getCrossComponentPredictionEnabledFlag())
      {
        const Int xOffset = rTu.getRect( COMPONENT_Y ).x0;
        const Int yOffset = rTu.getRect( COMPONENT_Y ).y0;
        for (UInt storedResidualIndex = 0; storedResidualIndex < NUMBER_OF_STORED_RESIDUAL_TYPES; storedResidualIndex++)
        {
          if (bMaintainResidual[storedResidualIndex])
          {
            xStoreCrossComponentPredictionResult(resiLuma[storedResidualIndex], resiLumaSplit[storedResidualIndex], rTu, xOffset, yOffset, MAX_CU_SIZE, MAX_CU_SIZE);
          }
        }
      }

      return;
    }

    //----- set entropy coding status -----
    m_pcRDGoOnSbacCoder->load ( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_TEST ] );

    //--- set transform index and Cbf values ---
    pcCU->setTrIdxSubParts( uiTrDepth, uiAbsPartIdx, uiFullDepth );
    const TComRectangle &tuRect=rTu.getRect(COMPONENT_Y);
    pcCU->setCbfSubParts  ( uiSingleCbfLuma << uiTrDepth, COMPONENT_Y, uiAbsPartIdx, totalAdjustedDepthChan );
    pcCU ->setTransformSkipSubParts  ( bestModeId[COMPONENT_Y], COMPONENT_Y, uiAbsPartIdx, totalAdjustedDepthChan );

    //--- set reconstruction for next intra prediction blocks ---
    const UInt  uiQTLayer   = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() - uiLog2TrSize;
    const UInt  uiZOrder    = pcCU->getZorderIdxInCtu() + uiAbsPartIdx;
    const UInt  uiWidth     = tuRect.width;
    const UInt  uiHeight    = tuRect.height;
    Pel*  piSrc       = m_pcQTTempTComYuv[ uiQTLayer ].getAddr( COMPONENT_Y, uiAbsPartIdx );
    UInt  uiSrcStride = m_pcQTTempTComYuv[ uiQTLayer ].getStride  ( COMPONENT_Y );
    Pel*  piDes       = pcCU->getPic()->getPicYuvRec()->getAddr( COMPONENT_Y, pcCU->getCtuRsAddr(), uiZOrder );
    UInt  uiDesStride = pcCU->getPic()->getPicYuvRec()->getStride  ( COMPONENT_Y );

    for( UInt uiY = 0; uiY < uiHeight; uiY++, piSrc += uiSrcStride, piDes += uiDesStride )
    {
      for( UInt uiX = 0; uiX < uiWidth; uiX++ )
      {
        piDes[ uiX ] = piSrc[ uiX ];
      }
    }
  }
  ruiDistY += uiSingleDistLuma;
  dRDCost  += dSingleCost;
}


Void
TEncSearch::xSetIntraResultLumaQT(TComYuv* pcRecoYuv, TComTU &rTu)
{
  TComDataCU *pcCU        = rTu.getCU();
  const UInt uiTrDepth    = rTu.GetTransformDepthRel();
  const UInt uiAbsPartIdx = rTu.GetAbsPartIdxTU();
  UInt uiTrMode     = pcCU->getTransformIdx( uiAbsPartIdx );
  if(  uiTrMode == uiTrDepth )
  {
    UInt uiLog2TrSize = rTu.GetLog2LumaTrSize();
    UInt uiQTLayer    = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() - uiLog2TrSize;

    //===== copy transform coefficients =====

    const TComRectangle &tuRect=rTu.getRect(COMPONENT_Y);
    const UInt coeffOffset = rTu.getCoefficientOffset(COMPONENT_Y);
    const UInt numCoeffInBlock = tuRect.width * tuRect.height;

    if (numCoeffInBlock!=0)
    {
      const TCoeff* srcCoeff = m_ppcQTTempCoeff[COMPONENT_Y][uiQTLayer] + coeffOffset;
      TCoeff* destCoeff      = pcCU->getCoeff(COMPONENT_Y) + coeffOffset;
      ::memcpy( destCoeff, srcCoeff, sizeof(TCoeff)*numCoeffInBlock );
#if ADAPTIVE_QP_SELECTION
      const TCoeff* srcArlCoeff = m_ppcQTTempArlCoeff[COMPONENT_Y][ uiQTLayer ] + coeffOffset;
      TCoeff* destArlCoeff      = pcCU->getArlCoeff (COMPONENT_Y)               + coeffOffset;
      ::memcpy( destArlCoeff, srcArlCoeff, sizeof( TCoeff ) * numCoeffInBlock );
#endif
      m_pcQTTempTComYuv[ uiQTLayer ].copyPartToPartComponent( COMPONENT_Y, pcRecoYuv, uiAbsPartIdx, tuRect.width, tuRect.height );
    }

  }
  else
  {
    TComTURecurse tuRecurseChild(rTu, false);
    do
    {
      xSetIntraResultLumaQT( pcRecoYuv, tuRecurseChild );
    } while (tuRecurseChild.nextSection(rTu));
  }
}


Void
TEncSearch::xStoreIntraResultQT(const ComponentID compID, TComTU &rTu )
{
  TComDataCU *pcCU=rTu.getCU();
  const UInt uiTrDepth = rTu.GetTransformDepthRel();
  const UInt uiAbsPartIdx = rTu.GetAbsPartIdxTU();
  const UInt uiTrMode     = pcCU->getTransformIdx( uiAbsPartIdx );
  if ( compID==COMPONENT_Y || uiTrMode == uiTrDepth )
  {
    assert(uiTrMode == uiTrDepth);
    const UInt uiLog2TrSize = rTu.GetLog2LumaTrSize();
    const UInt uiQTLayer    = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() - uiLog2TrSize;

    if (rTu.ProcessComponentSection(compID))
    {
      const TComRectangle &tuRect=rTu.getRect(compID);

      //===== copy transform coefficients =====
      const UInt uiNumCoeff    = tuRect.width * tuRect.height;
      TCoeff* pcCoeffSrc = m_ppcQTTempCoeff[compID] [ uiQTLayer ] + rTu.getCoefficientOffset(compID);
      TCoeff* pcCoeffDst = m_pcQTTempTUCoeff[compID];

      ::memcpy( pcCoeffDst, pcCoeffSrc, sizeof( TCoeff ) * uiNumCoeff );
#if ADAPTIVE_QP_SELECTION
      TCoeff* pcArlCoeffSrc = m_ppcQTTempArlCoeff[compID] [ uiQTLayer ] + rTu.getCoefficientOffset(compID);
      TCoeff* pcArlCoeffDst = m_ppcQTTempTUArlCoeff[compID];
      ::memcpy( pcArlCoeffDst, pcArlCoeffSrc, sizeof( TCoeff ) * uiNumCoeff );
#endif
      //===== copy reconstruction =====
      m_pcQTTempTComYuv[ uiQTLayer ].copyPartToPartComponent( compID, &m_pcQTTempTransformSkipTComYuv, uiAbsPartIdx, tuRect.width, tuRect.height );
    }
  }
}


Void
TEncSearch::xLoadIntraResultQT(const ComponentID compID, TComTU &rTu)
{
  TComDataCU *pcCU=rTu.getCU();
  const UInt uiTrDepth = rTu.GetTransformDepthRel();
  const UInt uiAbsPartIdx = rTu.GetAbsPartIdxTU();
  const UInt uiTrMode     = pcCU->getTransformIdx( uiAbsPartIdx );
  if ( compID==COMPONENT_Y || uiTrMode == uiTrDepth )
  {
    assert(uiTrMode == uiTrDepth);
    const UInt uiLog2TrSize = rTu.GetLog2LumaTrSize();
    const UInt uiQTLayer    = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() - uiLog2TrSize;
    const UInt uiZOrder     = pcCU->getZorderIdxInCtu() + uiAbsPartIdx;

    if (rTu.ProcessComponentSection(compID))
    {
      const TComRectangle &tuRect=rTu.getRect(compID);

      //===== copy transform coefficients =====
      const UInt uiNumCoeff = tuRect.width * tuRect.height;
      TCoeff* pcCoeffDst = m_ppcQTTempCoeff[compID] [ uiQTLayer ] + rTu.getCoefficientOffset(compID);
      TCoeff* pcCoeffSrc = m_pcQTTempTUCoeff[compID];

      ::memcpy( pcCoeffDst, pcCoeffSrc, sizeof( TCoeff ) * uiNumCoeff );
#if ADAPTIVE_QP_SELECTION
      TCoeff* pcArlCoeffDst = m_ppcQTTempArlCoeff[compID] [ uiQTLayer ] + rTu.getCoefficientOffset(compID);
      TCoeff* pcArlCoeffSrc = m_ppcQTTempTUArlCoeff[compID];
      ::memcpy( pcArlCoeffDst, pcArlCoeffSrc, sizeof( TCoeff ) * uiNumCoeff );
#endif
      //===== copy reconstruction =====
      m_pcQTTempTransformSkipTComYuv.copyPartToPartComponent( compID, &m_pcQTTempTComYuv[ uiQTLayer ], uiAbsPartIdx, tuRect.width, tuRect.height );

      Pel*    piRecIPred        = pcCU->getPic()->getPicYuvRec()->getAddr( compID, pcCU->getCtuRsAddr(), uiZOrder );
      UInt    uiRecIPredStride  = pcCU->getPic()->getPicYuvRec()->getStride (compID);
      Pel*    piRecQt           = m_pcQTTempTComYuv[ uiQTLayer ].getAddr( compID, uiAbsPartIdx );
      UInt    uiRecQtStride     = m_pcQTTempTComYuv[ uiQTLayer ].getStride  (compID);
      UInt    uiWidth           = tuRect.width;
      UInt    uiHeight          = tuRect.height;
      Pel* pRecQt               = piRecQt;
      Pel* pRecIPred            = piRecIPred;
      for( UInt uiY = 0; uiY < uiHeight; uiY++ )
      {
        for( UInt uiX = 0; uiX < uiWidth; uiX++ )
        {
          pRecIPred[ uiX ] = pRecQt   [ uiX ];
        }
        pRecQt    += uiRecQtStride;
        pRecIPred += uiRecIPredStride;
      }
    }
  }
}

Void
TEncSearch::xStoreCrossComponentPredictionResult(       Pel    *pResiDst,
                                                  const Pel    *pResiSrc,
                                                        TComTU &rTu,
                                                  const Int     xOffset,
                                                  const Int     yOffset,
                                                  const Int     strideDst,
                                                  const Int     strideSrc )
{
  const Pel *pSrc = pResiSrc + yOffset * strideSrc + xOffset;
        Pel *pDst = pResiDst + yOffset * strideDst + xOffset;

  for( Int y = 0; y < rTu.getRect( COMPONENT_Y ).height; y++ )
  {
    ::memcpy( pDst, pSrc, sizeof(Pel) * rTu.getRect( COMPONENT_Y ).width );
    pDst += strideDst;
    pSrc += strideSrc;
  }
}

SChar
TEncSearch::xCalcCrossComponentPredictionAlpha(       TComTU &rTu,
                                                const ComponentID compID,
                                                const Pel*        piResiL,
                                                const Pel*        piResiC,
                                                const Int         width,
                                                const Int         height,
                                                const Int         strideL,
                                                const Int         strideC )
{
  const Pel *pResiL = piResiL;
  const Pel *pResiC = piResiC;

        TComDataCU *pCU = rTu.getCU();
  const Int  absPartIdx = rTu.GetAbsPartIdxTU( compID );
  const Int diffBitDepth = pCU->getSlice()->getSPS()->getDifferentialLumaChromaBitDepth();

  SChar alpha = 0;
  Int SSxy  = 0;
  Int SSxx  = 0;

  for( UInt uiY = 0; uiY < height; uiY++ )
  {
    for( UInt uiX = 0; uiX < width; uiX++ )
    {
      const Pel scaledResiL = rightShift( pResiL[ uiX ], diffBitDepth );
      SSxy += ( scaledResiL * pResiC[ uiX ] );
      SSxx += ( scaledResiL * scaledResiL   );
    }

    pResiL += strideL;
    pResiC += strideC;
  }

  if( SSxx != 0 )
  {
    Double dAlpha = SSxy / Double( SSxx );
    alpha = SChar(Clip3<Int>(-16, 16, (Int)(dAlpha * 16)));

    static const SChar alphaQuant[17] = {0, 1, 1, 2, 2, 2, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8};

    alpha = (alpha < 0) ? -alphaQuant[Int(-alpha)] : alphaQuant[Int(alpha)];
  }
  pCU->setCrossComponentPredictionAlphaPartRange( alpha, compID, absPartIdx, rTu.GetAbsPartIdxNumParts( compID ) );

  return alpha;
}

Void
TEncSearch::xRecurIntraChromaCodingQT(TComYuv*    pcOrgYuv,
                                      TComYuv*    pcPredYuv,
                                      TComYuv*    pcResiYuv,
                                      Pel         resiLuma[NUMBER_OF_STORED_RESIDUAL_TYPES][MAX_CU_SIZE * MAX_CU_SIZE],
                                      Distortion& ruiDist,
                                      TComTU&     rTu
                                      DEBUG_STRING_FN_DECLARE(sDebug))
{
  TComDataCU         *pcCU                  = rTu.getCU();
  const UInt          uiTrDepth             = rTu.GetTransformDepthRel();
  const UInt          uiAbsPartIdx          = rTu.GetAbsPartIdxTU();
  const ChromaFormat  format                = rTu.GetChromaFormat();
  UInt                uiTrMode              = pcCU->getTransformIdx( uiAbsPartIdx );
  const UInt          numberValidComponents = getNumberValidComponents(format);

  if(  uiTrMode == uiTrDepth )
  {
    if (!rTu.ProcessChannelSection(CHANNEL_TYPE_CHROMA))
    {
      return;
    }

    const UInt uiFullDepth = rTu.GetTransformDepthTotal();

    Bool checkTransformSkip = pcCU->getSlice()->getPPS()->getUseTransformSkip();
    checkTransformSkip &= TUCompRectHasAssociatedTransformSkipFlag(rTu.getRect(COMPONENT_Cb), pcCU->getSlice()->getPPS()->getPpsRangeExtension().getLog2MaxTransformSkipBlockSize());

    if ( m_pcEncCfg->getUseTransformSkipFast() )
    {
      checkTransformSkip &= TUCompRectHasAssociatedTransformSkipFlag(rTu.getRect(COMPONENT_Y), pcCU->getSlice()->getPPS()->getPpsRangeExtension().getLog2MaxTransformSkipBlockSize());

      if (checkTransformSkip)
      {
        Int nbLumaSkip = 0;
        const UInt maxAbsPartIdxSub=uiAbsPartIdx + (rTu.ProcessingAllQuadrants(COMPONENT_Cb)?1:4);
        for(UInt absPartIdxSub = uiAbsPartIdx; absPartIdxSub < maxAbsPartIdxSub; absPartIdxSub ++)
        {
          nbLumaSkip += pcCU->getTransformSkip(absPartIdxSub, COMPONENT_Y);
        }
        checkTransformSkip &= (nbLumaSkip > 0);
      }
    }


    for (UInt ch=COMPONENT_Cb; ch<numberValidComponents; ch++)
    {
      const ComponentID compID = ComponentID(ch);
      DEBUG_STRING_NEW(sDebugBestMode)

      //use RDO to decide whether Cr/Cb takes TS
      m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[uiFullDepth][CI_QT_TRAFO_ROOT] );

      const Bool splitIntoSubTUs = rTu.getRect(compID).width != rTu.getRect(compID).height;

      TComTURecurse TUIterator(rTu, false, (splitIntoSubTUs ? TComTU::VERTICAL_SPLIT : TComTU::DONT_SPLIT), true, compID);

      const UInt partIdxesPerSubTU = TUIterator.GetAbsPartIdxNumParts(compID);

      do
      {
        const UInt subTUAbsPartIdx   = TUIterator.GetAbsPartIdxTU(compID);

        Double     dSingleCost               = MAX_DOUBLE;
        Int        bestModeId                = 0;
        Distortion singleDistC               = 0;
        UInt       singleCbfC                = 0;
        Distortion singleDistCTmp            = 0;
        Double     singleCostTmp             = 0;
        UInt       singleCbfCTmp             = 0;
        SChar      bestCrossCPredictionAlpha = 0;
        Int        bestTransformSkipMode     = 0;

        const Bool checkCrossComponentPrediction =    (pcCU->getIntraDir(CHANNEL_TYPE_CHROMA, subTUAbsPartIdx) == DM_CHROMA_IDX)
                                                   &&  pcCU->getSlice()->getPPS()->getPpsRangeExtension().getCrossComponentPredictionEnabledFlag()
                                                   && (pcCU->getCbf(subTUAbsPartIdx,  COMPONENT_Y, uiTrDepth) != 0);

        const Int  crossCPredictionModesToTest = checkCrossComponentPrediction ? 2 : 1;
        const Int  transformSkipModesToTest    = checkTransformSkip            ? 2 : 1;
        const Int  totalModesToTest            = crossCPredictionModesToTest * transformSkipModesToTest;
              Int  currModeId                  = 0;
              Int  default0Save1Load2          = 0;

        for(Int transformSkipModeId = 0; transformSkipModeId < transformSkipModesToTest; transformSkipModeId++)
        {
          for(Int crossCPredictionModeId = 0; crossCPredictionModeId < crossCPredictionModesToTest; crossCPredictionModeId++)
          {
            pcCU->setCrossComponentPredictionAlphaPartRange(0, compID, subTUAbsPartIdx, partIdxesPerSubTU);
            DEBUG_STRING_NEW(sDebugMode)
            pcCU->setTransformSkipPartRange( transformSkipModeId, compID, subTUAbsPartIdx, partIdxesPerSubTU );
            currModeId++;

            const Bool isOneMode  = (totalModesToTest == 1);
            const Bool isLastMode = (currModeId == totalModesToTest); // currModeId is indexed from 1

            if (isOneMode)
            {
              default0Save1Load2 = 0;
            }
            else if (!isOneMode && (transformSkipModeId == 0) && (crossCPredictionModeId == 0))
            {
              default0Save1Load2 = 1; //save prediction on first mode
            }
            else
            {
              default0Save1Load2 = 2; //load it on subsequent modes
            }

            singleDistCTmp = 0;

            xIntraCodingTUBlock( pcOrgYuv, pcPredYuv, pcResiYuv, resiLuma, (crossCPredictionModeId != 0), singleDistCTmp, compID, TUIterator DEBUG_STRING_PASS_INTO(sDebugMode), default0Save1Load2);
            singleCbfCTmp = pcCU->getCbf( subTUAbsPartIdx, compID, uiTrDepth);

            if (  ((crossCPredictionModeId == 1) && (pcCU->getCrossComponentPredictionAlpha(subTUAbsPartIdx, compID) == 0))
               || ((transformSkipModeId    == 1) && (singleCbfCTmp == 0))) //In order not to code TS flag when cbf is zero, the case for TS with cbf being zero is forbidden.
            {
              singleCostTmp = MAX_DOUBLE;
            }
            else if (!isOneMode)
            {
              UInt bitsTmp = xGetIntraBitsQTChroma( TUIterator, compID, false );
              singleCostTmp  = m_pcRdCost->calcRdCost( bitsTmp, singleDistCTmp);
            }

            if(singleCostTmp < dSingleCost)
            {
              DEBUG_STRING_SWAP(sDebugBestMode, sDebugMode)
              dSingleCost               = singleCostTmp;
              singleDistC               = singleDistCTmp;
              bestCrossCPredictionAlpha = (crossCPredictionModeId != 0) ? pcCU->getCrossComponentPredictionAlpha(subTUAbsPartIdx, compID) : 0;
              bestTransformSkipMode     = transformSkipModeId;
              bestModeId                = currModeId;
              singleCbfC                = singleCbfCTmp;

              if (!isOneMode && !isLastMode)
              {
                xStoreIntraResultQT(compID, TUIterator);
                m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ uiFullDepth ][ CI_TEMP_BEST ] );
              }
            }

            if (!isOneMode && !isLastMode)
            {
              m_pcRDGoOnSbacCoder->load ( m_pppcRDSbacCoder[ uiFullDepth ][ CI_QT_TRAFO_ROOT ] );
            }
          }
        }

        if(bestModeId < totalModesToTest)
        {
          xLoadIntraResultQT(compID, TUIterator);
          pcCU->setCbfPartRange( singleCbfC << uiTrDepth, compID, subTUAbsPartIdx, partIdxesPerSubTU );

          m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[ uiFullDepth ][ CI_TEMP_BEST ] );
        }

        DEBUG_STRING_APPEND(sDebug, sDebugBestMode)
        pcCU ->setTransformSkipPartRange                ( bestTransformSkipMode,     compID, subTUAbsPartIdx, partIdxesPerSubTU );
        pcCU ->setCrossComponentPredictionAlphaPartRange( bestCrossCPredictionAlpha, compID, subTUAbsPartIdx, partIdxesPerSubTU );
        ruiDist += singleDistC;
      } while (TUIterator.nextSection(rTu));

      if (splitIntoSubTUs)
      {
        offsetSubTUCBFs(rTu, compID);
      }
    }
  }
  else
  {
    UInt    uiSplitCbf[MAX_NUM_COMPONENT] = {0,0,0};

    TComTURecurse tuRecurseChild(rTu, false);
    const UInt uiTrDepthChild   = tuRecurseChild.GetTransformDepthRel();
    do
    {
      DEBUG_STRING_NEW(sChild)

      xRecurIntraChromaCodingQT( pcOrgYuv, pcPredYuv, pcResiYuv, resiLuma, ruiDist, tuRecurseChild DEBUG_STRING_PASS_INTO(sChild) );

      DEBUG_STRING_APPEND(sDebug, sChild)
      const UInt uiAbsPartIdxSub=tuRecurseChild.GetAbsPartIdxTU();

      for(UInt ch=COMPONENT_Cb; ch<numberValidComponents; ch++)
      {
        uiSplitCbf[ch] |= pcCU->getCbf( uiAbsPartIdxSub, ComponentID(ch), uiTrDepthChild );
      }
    } while ( tuRecurseChild.nextSection(rTu) );


    UInt uiPartsDiv = rTu.GetAbsPartIdxNumParts();
    for(UInt ch=COMPONENT_Cb; ch<numberValidComponents; ch++)
    {
      if (uiSplitCbf[ch])
      {
        const UInt flag=1<<uiTrDepth;
        ComponentID compID=ComponentID(ch);
        UChar *pBase=pcCU->getCbf( compID );
        for( UInt uiOffs = 0; uiOffs < uiPartsDiv; uiOffs++ )
        {
          pBase[ uiAbsPartIdx + uiOffs ] |= flag;
        }
      }
    }
  }
}




Void
TEncSearch::xSetIntraResultChromaQT(TComYuv*    pcRecoYuv, TComTU &rTu)
{
  if (!rTu.ProcessChannelSection(CHANNEL_TYPE_CHROMA))
  {
    return;
  }
  TComDataCU *pcCU=rTu.getCU();
  const UInt uiAbsPartIdx = rTu.GetAbsPartIdxTU();
  const UInt uiTrDepth   = rTu.GetTransformDepthRel();
  UInt uiTrMode     = pcCU->getTransformIdx( uiAbsPartIdx );
  if(  uiTrMode == uiTrDepth )
  {
    UInt uiLog2TrSize = rTu.GetLog2LumaTrSize();
    UInt uiQTLayer    = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() - uiLog2TrSize;

    //===== copy transform coefficients =====
    const TComRectangle &tuRectCb=rTu.getRect(COMPONENT_Cb);
    UInt uiNumCoeffC    = tuRectCb.width*tuRectCb.height;//( pcCU->getSlice()->getSPS()->getMaxCUWidth() * pcCU->getSlice()->getSPS()->getMaxCUHeight() ) >> ( uiFullDepth << 1 );
    const UInt offset = rTu.getCoefficientOffset(COMPONENT_Cb);

    const UInt numberValidComponents = getNumberValidComponents(rTu.GetChromaFormat());
    for (UInt ch=COMPONENT_Cb; ch<numberValidComponents; ch++)
    {
      const ComponentID component = ComponentID(ch);
      const TCoeff* src           = m_ppcQTTempCoeff[component][uiQTLayer] + offset;//(uiNumCoeffIncC*uiAbsPartIdx);
      TCoeff* dest                = pcCU->getCoeff(component) + offset;//(uiNumCoeffIncC*uiAbsPartIdx);
      ::memcpy( dest, src, sizeof(TCoeff)*uiNumCoeffC );
#if ADAPTIVE_QP_SELECTION
      TCoeff* pcArlCoeffSrc = m_ppcQTTempArlCoeff[component][ uiQTLayer ] + offset;//( uiNumCoeffIncC * uiAbsPartIdx );
      TCoeff* pcArlCoeffDst = pcCU->getArlCoeff(component)                + offset;//( uiNumCoeffIncC * uiAbsPartIdx );
      ::memcpy( pcArlCoeffDst, pcArlCoeffSrc, sizeof( TCoeff ) * uiNumCoeffC );
#endif
    }

    //===== copy reconstruction =====

    m_pcQTTempTComYuv[ uiQTLayer ].copyPartToPartComponent( COMPONENT_Cb, pcRecoYuv, uiAbsPartIdx, tuRectCb.width, tuRectCb.height );
    m_pcQTTempTComYuv[ uiQTLayer ].copyPartToPartComponent( COMPONENT_Cr, pcRecoYuv, uiAbsPartIdx, tuRectCb.width, tuRectCb.height );
  }
  else
  {
    TComTURecurse tuRecurseChild(rTu, false);
    do
    {
      xSetIntraResultChromaQT( pcRecoYuv, tuRecurseChild );
    } while (tuRecurseChild.nextSection(rTu));
  }
}



Void
TEncSearch::estIntraPredLumaQT(TComDataCU* pcCU,
                               TComYuv*    pcOrgYuv,
                               TComYuv*    pcPredYuv,
                               TComYuv*    pcResiYuv,
                               TComYuv*    pcRecoYuv,
                               Pel         resiLuma[NUMBER_OF_STORED_RESIDUAL_TYPES][MAX_CU_SIZE * MAX_CU_SIZE]
                               DEBUG_STRING_FN_DECLARE(sDebug))
{
  const UInt         uiDepth               = pcCU->getDepth(0);
  const UInt         uiInitTrDepth         = pcCU->getPartitionSize(0) == SIZE_2Nx2N ? 0 : 1;
  const UInt         uiNumPU               = 1<<(2*uiInitTrDepth);
  const UInt         uiQNumParts           = pcCU->getTotalNumPart() >> 2;
  const UInt         uiWidthBit            = pcCU->getIntraSizeIdx(0);
  const ChromaFormat chFmt                 = pcCU->getPic()->getChromaFormat();
  const UInt         numberValidComponents = getNumberValidComponents(chFmt);
  const TComSPS     &sps                   = *(pcCU->getSlice()->getSPS());
  const TComPPS     &pps                   = *(pcCU->getSlice()->getPPS());
        Distortion   uiOverallDistY        = 0;
        UInt         CandNum;
        Double       CandCostList[ FAST_UDI_MAX_RDMODE_NUM ];
        Pel          resiLumaPU[NUMBER_OF_STORED_RESIDUAL_TYPES][MAX_CU_SIZE * MAX_CU_SIZE];

        Bool    bMaintainResidual[NUMBER_OF_STORED_RESIDUAL_TYPES];
        for (UInt residualTypeIndex = 0; residualTypeIndex < NUMBER_OF_STORED_RESIDUAL_TYPES; residualTypeIndex++)
        {
          bMaintainResidual[residualTypeIndex] = true; //assume true unless specified otherwise
        }

        bMaintainResidual[RESIDUAL_ENCODER_SIDE] = !(m_pcEncCfg->getUseReconBasedCrossCPredictionEstimate());

  // Lambda calculation at equivalent Qp of 4 is recommended because at that Qp, the quantisation divisor is 1.
#if FULL_NBIT
  const Double sqrtLambdaForFirstPass= (m_pcEncCfg->getCostMode()==COST_MIXED_LOSSLESS_LOSSY_CODING && pcCU->getCUTransquantBypass(0)) ?
                sqrt(0.57 * pow(2.0, ((LOSSLESS_AND_MIXED_LOSSLESS_RD_COST_TEST_QP_PRIME - 12) / 3.0)))
              : m_pcRdCost->getSqrtLambda();
#else
  const Double sqrtLambdaForFirstPass= (m_pcEncCfg->getCostMode()==COST_MIXED_LOSSLESS_LOSSY_CODING && pcCU->getCUTransquantBypass(0)) ?
                sqrt(0.57 * pow(2.0, ((LOSSLESS_AND_MIXED_LOSSLESS_RD_COST_TEST_QP_PRIME - 12 - 6 * (sps.getBitDepth(CHANNEL_TYPE_LUMA) - 8)) / 3.0)))
              : m_pcRdCost->getSqrtLambda();
#endif

  //===== set QP and clear Cbf =====
  if ( pps.getUseDQP() == true)
  {
    pcCU->setQPSubParts( pcCU->getQP(0), 0, uiDepth );
  }
  else
  {
    pcCU->setQPSubParts( pcCU->getSlice()->getSliceQp(), 0, uiDepth );
  }

  //===== loop over partitions =====
  TComTURecurse tuRecurseCU(pcCU, 0);
  TComTURecurse tuRecurseWithPU(tuRecurseCU, false, (uiInitTrDepth==0)?TComTU::DONT_SPLIT : TComTU::QUAD_SPLIT);

  do
  {
    const UInt uiPartOffset=tuRecurseWithPU.GetAbsPartIdxTU();
//  for( UInt uiPU = 0, uiPartOffset=0; uiPU < uiNumPU; uiPU++, uiPartOffset += uiQNumParts )
  //{
    //===== init pattern for luma prediction =====
    DEBUG_STRING_NEW(sTemp2)

    //===== determine set of modes to be tested (using prediction signal only) =====
    Int numModesAvailable     = 35; //total number of Intra modes
    UInt uiRdModeList[FAST_UDI_MAX_RDMODE_NUM];
    Int numModesForFullRD = m_pcEncCfg->getFastUDIUseMPMEnabled()?g_aucIntraModeNumFast_UseMPM[ uiWidthBit ] : g_aucIntraModeNumFast_NotUseMPM[ uiWidthBit ];

    // this should always be true
    assert (tuRecurseWithPU.ProcessComponentSection(COMPONENT_Y));
    initIntraPatternChType( tuRecurseWithPU, COMPONENT_Y, true DEBUG_STRING_PASS_INTO(sTemp2) );

    Bool doFastSearch = (numModesForFullRD != numModesAvailable);
    if (doFastSearch)
    {
      assert(numModesForFullRD < numModesAvailable);

      for( Int i=0; i < numModesForFullRD; i++ )
      {
        CandCostList[ i ] = MAX_DOUBLE;
      }
      CandNum = 0;

      const TComRectangle &puRect=tuRecurseWithPU.getRect(COMPONENT_Y);
      const UInt uiAbsPartIdx=tuRecurseWithPU.GetAbsPartIdxTU();

      Pel* piOrg         = pcOrgYuv ->getAddr( COMPONENT_Y, uiAbsPartIdx );
      Pel* piPred        = pcPredYuv->getAddr( COMPONENT_Y, uiAbsPartIdx );
      UInt uiStride      = pcPredYuv->getStride( COMPONENT_Y );
      DistParam distParam;
      const Bool bUseHadamard=pcCU->getCUTransquantBypass(0) == 0;
      m_pcRdCost->setDistParam(distParam, sps.getBitDepth(CHANNEL_TYPE_LUMA), piOrg, uiStride, piPred, uiStride, puRect.width, puRect.height, bUseHadamard);
      distParam.bApplyWeight = false;
      for( Int modeIdx = 0; modeIdx < numModesAvailable; modeIdx++ )
      {
        UInt       uiMode = modeIdx;
        Distortion uiSad  = 0;

        const Bool bUseFilter=TComPrediction::filteringIntraReferenceSamples(COMPONENT_Y, uiMode, puRect.width, puRect.height, chFmt, sps.getSpsRangeExtension().getIntraSmoothingDisabledFlag());

        predIntraAng( COMPONENT_Y, uiMode, piOrg, uiStride, piPred, uiStride, tuRecurseWithPU, bUseFilter, TComPrediction::UseDPCMForFirstPassIntraEstimation(tuRecurseWithPU, uiMode) );

        // use hadamard transform here
        uiSad+=distParam.DistFunc(&distParam);

        UInt   iModeBits = 0;

        // NB xModeBitsIntra will not affect the mode for chroma that may have already been pre-estimated.
        iModeBits+=xModeBitsIntra( pcCU, uiMode, uiPartOffset, uiDepth, CHANNEL_TYPE_LUMA );

        Double cost      = (Double)uiSad + (Double)iModeBits * sqrtLambdaForFirstPass;

#if DEBUG_INTRA_SEARCH_COSTS
        std::cout << "1st pass mode " << uiMode << " SAD = " << uiSad << ", mode bits = " << iModeBits << ", cost = " << cost << "\n";
#endif

        CandNum += xUpdateCandList( uiMode, cost, numModesForFullRD, uiRdModeList, CandCostList );
      }

      if (m_pcEncCfg->getFastUDIUseMPMEnabled())
      {
        Int uiPreds[NUM_MOST_PROBABLE_MODES] = {-1, -1, -1};

        Int iMode = -1;
        pcCU->getIntraDirPredictor( uiPartOffset, uiPreds, COMPONENT_Y, &iMode );

        const Int numCand = ( iMode >= 0 ) ? iMode : Int(NUM_MOST_PROBABLE_MODES);

        for( Int j=0; j < numCand; j++)
        {
          Bool mostProbableModeIncluded = false;
          Int mostProbableMode = uiPreds[j];

          for( Int i=0; i < numModesForFullRD; i++)
          {
            mostProbableModeIncluded |= (mostProbableMode == uiRdModeList[i]);
          }
          if (!mostProbableModeIncluded)
          {
            uiRdModeList[numModesForFullRD++] = mostProbableMode;
          }
        }
      }
    }
    else
    {
      for( Int i=0; i < numModesForFullRD; i++)
      {
        uiRdModeList[i] = i;
      }
    }

    //===== check modes (using r-d costs) =====
#if HHI_RQT_INTRA_SPEEDUP_MOD
    UInt   uiSecondBestMode  = MAX_UINT;
    Double dSecondBestPUCost = MAX_DOUBLE;
#endif
    DEBUG_STRING_NEW(sPU)
    UInt       uiBestPUMode  = 0;
    Distortion uiBestPUDistY = 0;
    Double     dBestPUCost   = MAX_DOUBLE;

#if ENVIRONMENT_VARIABLE_DEBUG_AND_TEST
    UInt max=numModesForFullRD;

    if (DebugOptionList::ForceLumaMode.isSet())
    {
      max=0;  // we are forcing a direction, so don't bother with mode check
    }
    for ( UInt uiMode = 0; uiMode < max; uiMode++)
#else
    for( UInt uiMode = 0; uiMode < numModesForFullRD; uiMode++ )
#endif
    {
      // set luma prediction mode
      UInt uiOrgMode = uiRdModeList[uiMode];

      pcCU->setIntraDirSubParts ( CHANNEL_TYPE_LUMA, uiOrgMode, uiPartOffset, uiDepth + uiInitTrDepth );

      DEBUG_STRING_NEW(sMode)
      // set context models
      m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[uiDepth][CI_CURR_BEST] );

      // determine residual for partition
      Distortion uiPUDistY = 0;
      Double     dPUCost   = 0.0;
#if HHI_RQT_INTRA_SPEEDUP
      xRecurIntraCodingLumaQT( pcOrgYuv, pcPredYuv, pcResiYuv, resiLumaPU, uiPUDistY, true, dPUCost, tuRecurseWithPU DEBUG_STRING_PASS_INTO(sMode) );
#else
      xRecurIntraCodingLumaQT( pcOrgYuv, pcPredYuv, pcResiYuv, resiLumaPU, uiPUDistY, dPUCost, tuRecurseWithPU DEBUG_STRING_PASS_INTO(sMode) );
#endif

#if DEBUG_INTRA_SEARCH_COSTS
      std::cout << "2nd pass [luma,chroma] mode [" << Int(pcCU->getIntraDir(CHANNEL_TYPE_LUMA, uiPartOffset)) << "," << Int(pcCU->getIntraDir(CHANNEL_TYPE_CHROMA, uiPartOffset)) << "] cost = " << dPUCost << "\n";
#endif

      // check r-d cost
      if( dPUCost < dBestPUCost )
      {
        DEBUG_STRING_SWAP(sPU, sMode)
#if HHI_RQT_INTRA_SPEEDUP_MOD
        uiSecondBestMode  = uiBestPUMode;
        dSecondBestPUCost = dBestPUCost;
#endif
        uiBestPUMode  = uiOrgMode;
        uiBestPUDistY = uiPUDistY;
        dBestPUCost   = dPUCost;

        xSetIntraResultLumaQT( pcRecoYuv, tuRecurseWithPU );

        if (pps.getPpsRangeExtension().getCrossComponentPredictionEnabledFlag())
        {
          const Int xOffset = tuRecurseWithPU.getRect( COMPONENT_Y ).x0;
          const Int yOffset = tuRecurseWithPU.getRect( COMPONENT_Y ).y0;
          for (UInt storedResidualIndex = 0; storedResidualIndex < NUMBER_OF_STORED_RESIDUAL_TYPES; storedResidualIndex++)
          {
            if (bMaintainResidual[storedResidualIndex])
            {
              xStoreCrossComponentPredictionResult(resiLuma[storedResidualIndex], resiLumaPU[storedResidualIndex], tuRecurseWithPU, xOffset, yOffset, MAX_CU_SIZE, MAX_CU_SIZE );
            }
          }
        }

        UInt uiQPartNum = tuRecurseWithPU.GetAbsPartIdxNumParts();

        ::memcpy( m_puhQTTempTrIdx,  pcCU->getTransformIdx()       + uiPartOffset, uiQPartNum * sizeof( UChar ) );
        for (UInt component = 0; component < numberValidComponents; component++)
        {
          const ComponentID compID = ComponentID(component);
          ::memcpy( m_puhQTTempCbf[compID], pcCU->getCbf( compID  ) + uiPartOffset, uiQPartNum * sizeof( UChar ) );
          ::memcpy( m_puhQTTempTransformSkipFlag[compID],  pcCU->getTransformSkip(compID)  + uiPartOffset, uiQPartNum * sizeof( UChar ) );
        }
      }
#if HHI_RQT_INTRA_SPEEDUP_MOD
      else if( dPUCost < dSecondBestPUCost )
      {
        uiSecondBestMode  = uiOrgMode;
        dSecondBestPUCost = dPUCost;
      }
#endif
    } // Mode loop

#if HHI_RQT_INTRA_SPEEDUP
#if HHI_RQT_INTRA_SPEEDUP_MOD
    for( UInt ui =0; ui < 2; ++ui )
#endif
    {
#if HHI_RQT_INTRA_SPEEDUP_MOD
      UInt uiOrgMode   = ui ? uiSecondBestMode  : uiBestPUMode;
      if( uiOrgMode == MAX_UINT )
      {
        break;
      }
#else
      UInt uiOrgMode = uiBestPUMode;
#endif

#if ENVIRONMENT_VARIABLE_DEBUG_AND_TEST
      if (DebugOptionList::ForceLumaMode.isSet())
      {
        uiOrgMode = DebugOptionList::ForceLumaMode.getInt();
      }
#endif

      pcCU->setIntraDirSubParts ( CHANNEL_TYPE_LUMA, uiOrgMode, uiPartOffset, uiDepth + uiInitTrDepth );
      DEBUG_STRING_NEW(sModeTree)

      // set context models
      m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[uiDepth][CI_CURR_BEST] );

      // determine residual for partition
      Distortion uiPUDistY = 0;
      Double     dPUCost   = 0.0;

      xRecurIntraCodingLumaQT( pcOrgYuv, pcPredYuv, pcResiYuv, resiLumaPU, uiPUDistY, false, dPUCost, tuRecurseWithPU DEBUG_STRING_PASS_INTO(sModeTree));

      // check r-d cost
      if( dPUCost < dBestPUCost )
      {
        DEBUG_STRING_SWAP(sPU, sModeTree)
        uiBestPUMode  = uiOrgMode;
        uiBestPUDistY = uiPUDistY;
        dBestPUCost   = dPUCost;

        xSetIntraResultLumaQT( pcRecoYuv, tuRecurseWithPU );

        if (pps.getPpsRangeExtension().getCrossComponentPredictionEnabledFlag())
        {
          const Int xOffset = tuRecurseWithPU.getRect( COMPONENT_Y ).x0;
          const Int yOffset = tuRecurseWithPU.getRect( COMPONENT_Y ).y0;
          for (UInt storedResidualIndex = 0; storedResidualIndex < NUMBER_OF_STORED_RESIDUAL_TYPES; storedResidualIndex++)
          {
            if (bMaintainResidual[storedResidualIndex])
            {
              xStoreCrossComponentPredictionResult(resiLuma[storedResidualIndex], resiLumaPU[storedResidualIndex], tuRecurseWithPU, xOffset, yOffset, MAX_CU_SIZE, MAX_CU_SIZE );
            }
          }
        }

        const UInt uiQPartNum = tuRecurseWithPU.GetAbsPartIdxNumParts();
        ::memcpy( m_puhQTTempTrIdx,  pcCU->getTransformIdx()       + uiPartOffset, uiQPartNum * sizeof( UChar ) );

        for (UInt component = 0; component < numberValidComponents; component++)
        {
          const ComponentID compID = ComponentID(component);
          ::memcpy( m_puhQTTempCbf[compID], pcCU->getCbf( compID  ) + uiPartOffset, uiQPartNum * sizeof( UChar ) );
          ::memcpy( m_puhQTTempTransformSkipFlag[compID],  pcCU->getTransformSkip(compID)  + uiPartOffset, uiQPartNum * sizeof( UChar ) );
        }
      }
    } // Mode loop
#endif

    DEBUG_STRING_APPEND(sDebug, sPU)

    //--- update overall distortion ---
    uiOverallDistY += uiBestPUDistY;

    //--- update transform index and cbf ---
    const UInt uiQPartNum = tuRecurseWithPU.GetAbsPartIdxNumParts();
    ::memcpy( pcCU->getTransformIdx()       + uiPartOffset, m_puhQTTempTrIdx,  uiQPartNum * sizeof( UChar ) );
    for (UInt component = 0; component < numberValidComponents; component++)
    {
      const ComponentID compID = ComponentID(component);
      ::memcpy( pcCU->getCbf( compID  ) + uiPartOffset, m_puhQTTempCbf[compID], uiQPartNum * sizeof( UChar ) );
      ::memcpy( pcCU->getTransformSkip( compID  ) + uiPartOffset, m_puhQTTempTransformSkipFlag[compID ], uiQPartNum * sizeof( UChar ) );
    }

    //--- set reconstruction for next intra prediction blocks ---
    if( !tuRecurseWithPU.IsLastSection() )
    {
      const TComRectangle &puRect=tuRecurseWithPU.getRect(COMPONENT_Y);
      const UInt  uiCompWidth   = puRect.width;
      const UInt  uiCompHeight  = puRect.height;

      const UInt  uiZOrder      = pcCU->getZorderIdxInCtu() + uiPartOffset;
            Pel*  piDes         = pcCU->getPic()->getPicYuvRec()->getAddr( COMPONENT_Y, pcCU->getCtuRsAddr(), uiZOrder );
      const UInt  uiDesStride   = pcCU->getPic()->getPicYuvRec()->getStride( COMPONENT_Y);
      const Pel*  piSrc         = pcRecoYuv->getAddr( COMPONENT_Y, uiPartOffset );
      const UInt  uiSrcStride   = pcRecoYuv->getStride( COMPONENT_Y);

      for( UInt uiY = 0; uiY < uiCompHeight; uiY++, piSrc += uiSrcStride, piDes += uiDesStride )
      {
        for( UInt uiX = 0; uiX < uiCompWidth; uiX++ )
        {
          piDes[ uiX ] = piSrc[ uiX ];
        }
      }
    }

    //=== update PU data ====
    pcCU->setIntraDirSubParts     ( CHANNEL_TYPE_LUMA, uiBestPUMode, uiPartOffset, uiDepth + uiInitTrDepth );
	
  } while (tuRecurseWithPU.nextSection(tuRecurseCU));


  if( uiNumPU > 1 )
  { // set Cbf for all blocks
    UInt uiCombCbfY = 0;
    UInt uiCombCbfU = 0;
    UInt uiCombCbfV = 0;
    UInt uiPartIdx  = 0;
    for( UInt uiPart = 0; uiPart < 4; uiPart++, uiPartIdx += uiQNumParts )
    {
      uiCombCbfY |= pcCU->getCbf( uiPartIdx, COMPONENT_Y,  1 );
      uiCombCbfU |= pcCU->getCbf( uiPartIdx, COMPONENT_Cb, 1 );
      uiCombCbfV |= pcCU->getCbf( uiPartIdx, COMPONENT_Cr, 1 );
    }
    for( UInt uiOffs = 0; uiOffs < 4 * uiQNumParts; uiOffs++ )
    {
      pcCU->getCbf( COMPONENT_Y  )[ uiOffs ] |= uiCombCbfY;
      pcCU->getCbf( COMPONENT_Cb )[ uiOffs ] |= uiCombCbfU;
      pcCU->getCbf( COMPONENT_Cr )[ uiOffs ] |= uiCombCbfV;
    }
  }

  //===== reset context models =====
  m_pcRDGoOnSbacCoder->load(m_pppcRDSbacCoder[uiDepth][CI_CURR_BEST]);

  //===== set distortion (rate and r-d costs are determined later) =====
  pcCU->getTotalDistortion() = uiOverallDistY;
}




Void
TEncSearch::estIntraPredChromaQT(TComDataCU* pcCU,
                                 TComYuv*    pcOrgYuv,
                                 TComYuv*    pcPredYuv,
                                 TComYuv*    pcResiYuv,
                                 TComYuv*    pcRecoYuv,
                                 Pel         resiLuma[NUMBER_OF_STORED_RESIDUAL_TYPES][MAX_CU_SIZE * MAX_CU_SIZE]
                                 DEBUG_STRING_FN_DECLARE(sDebug))
{
  const UInt    uiInitTrDepth  = pcCU->getPartitionSize(0) != SIZE_2Nx2N && enable4ChromaPUsInIntraNxNCU(pcOrgYuv->getChromaFormat()) ? 1 : 0;

  TComTURecurse tuRecurseCU(pcCU, 0);
  TComTURecurse tuRecurseWithPU(tuRecurseCU, false, (uiInitTrDepth==0)?TComTU::DONT_SPLIT : TComTU::QUAD_SPLIT);
  const UInt    uiQNumParts    = tuRecurseWithPU.GetAbsPartIdxNumParts();
  const UInt    uiDepthCU=tuRecurseWithPU.getCUDepth();
  const UInt    numberValidComponents = pcCU->getPic()->getNumberValidComponents();

  do
  {
    UInt       uiBestMode  = 0;
    Distortion uiBestDist  = 0;
    Double     dBestCost   = MAX_DOUBLE;

    //----- init mode list -----
    if (tuRecurseWithPU.ProcessChannelSection(CHANNEL_TYPE_CHROMA))
    {
      UInt uiModeList[FAST_UDI_MAX_RDMODE_NUM];
      const UInt  uiQPartNum     = uiQNumParts;
      const UInt  uiPartOffset   = tuRecurseWithPU.GetAbsPartIdxTU();
      {
        UInt  uiMinMode = 0;
        UInt  uiMaxMode = NUM_CHROMA_MODE;

        //----- check chroma modes -----
        pcCU->getAllowedChromaDir( uiPartOffset, uiModeList );

#if ENVIRONMENT_VARIABLE_DEBUG_AND_TEST
        if (DebugOptionList::ForceChromaMode.isSet())
        {
          uiMinMode=DebugOptionList::ForceChromaMode.getInt();
          if (uiModeList[uiMinMode]==34)
          {
            uiMinMode=4; // if the fixed mode has been renumbered because DM_CHROMA covers it, use DM_CHROMA.
          }
          uiMaxMode=uiMinMode+1;
        }
#endif

        DEBUG_STRING_NEW(sPU)

        for( UInt uiMode = uiMinMode; uiMode < uiMaxMode; uiMode++ )
        {
          //----- restore context models -----
          m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[uiDepthCU][CI_CURR_BEST] );
          
          DEBUG_STRING_NEW(sMode)
          //----- chroma coding -----
          Distortion uiDist = 0;
          pcCU->setIntraDirSubParts  ( CHANNEL_TYPE_CHROMA, uiModeList[uiMode], uiPartOffset, uiDepthCU+uiInitTrDepth );
          xRecurIntraChromaCodingQT       ( pcOrgYuv, pcPredYuv, pcResiYuv, resiLuma, uiDist, tuRecurseWithPU DEBUG_STRING_PASS_INTO(sMode) );

          if( pcCU->getSlice()->getPPS()->getUseTransformSkip() )
          {
            m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[uiDepthCU][CI_CURR_BEST] );
          }

          UInt    uiBits = xGetIntraBitsQT( tuRecurseWithPU, false, true, false );
          Double  dCost  = m_pcRdCost->calcRdCost( uiBits, uiDist );

          //----- compare -----
          if( dCost < dBestCost )
          {
            DEBUG_STRING_SWAP(sPU, sMode);
            dBestCost   = dCost;
            uiBestDist  = uiDist;
            uiBestMode  = uiModeList[uiMode];

            xSetIntraResultChromaQT( pcRecoYuv, tuRecurseWithPU );
            for (UInt componentIndex = COMPONENT_Cb; componentIndex < numberValidComponents; componentIndex++)
            {
              const ComponentID compID = ComponentID(componentIndex);
              ::memcpy( m_puhQTTempCbf[compID], pcCU->getCbf( compID )+uiPartOffset, uiQPartNum * sizeof( UChar ) );
              ::memcpy( m_puhQTTempTransformSkipFlag[compID], pcCU->getTransformSkip( compID )+uiPartOffset, uiQPartNum * sizeof( UChar ) );
              ::memcpy( m_phQTTempCrossComponentPredictionAlpha[compID], pcCU->getCrossComponentPredictionAlpha(compID)+uiPartOffset, uiQPartNum * sizeof( SChar ) );
            }
          }
        }

        DEBUG_STRING_APPEND(sDebug, sPU)

        //----- set data -----
        for (UInt componentIndex = COMPONENT_Cb; componentIndex < numberValidComponents; componentIndex++)
        {
          const ComponentID compID = ComponentID(componentIndex);
          ::memcpy( pcCU->getCbf( compID )+uiPartOffset, m_puhQTTempCbf[compID], uiQPartNum * sizeof( UChar ) );
          ::memcpy( pcCU->getTransformSkip( compID )+uiPartOffset, m_puhQTTempTransformSkipFlag[compID], uiQPartNum * sizeof( UChar ) );
          ::memcpy( pcCU->getCrossComponentPredictionAlpha(compID)+uiPartOffset, m_phQTTempCrossComponentPredictionAlpha[compID], uiQPartNum * sizeof( SChar ) );
        }
      }

      if( ! tuRecurseWithPU.IsLastSection() )
      {
        for (UInt ch=COMPONENT_Cb; ch<numberValidComponents; ch++)
        {
          const ComponentID compID    = ComponentID(ch);
          const TComRectangle &tuRect = tuRecurseWithPU.getRect(compID);
          const UInt  uiCompWidth     = tuRect.width;
          const UInt  uiCompHeight    = tuRect.height;
          const UInt  uiZOrder        = pcCU->getZorderIdxInCtu() + tuRecurseWithPU.GetAbsPartIdxTU();
                Pel*  piDes           = pcCU->getPic()->getPicYuvRec()->getAddr( compID, pcCU->getCtuRsAddr(), uiZOrder );
          const UInt  uiDesStride     = pcCU->getPic()->getPicYuvRec()->getStride( compID);
          const Pel*  piSrc           = pcRecoYuv->getAddr( compID, uiPartOffset );
          const UInt  uiSrcStride     = pcRecoYuv->getStride( compID);

          for( UInt uiY = 0; uiY < uiCompHeight; uiY++, piSrc += uiSrcStride, piDes += uiDesStride )
          {
            for( UInt uiX = 0; uiX < uiCompWidth; uiX++ )
            {
              piDes[ uiX ] = piSrc[ uiX ];
            }
          }
        }
      }

      pcCU->setIntraDirSubParts( CHANNEL_TYPE_CHROMA, uiBestMode, uiPartOffset, uiDepthCU+uiInitTrDepth );
      pcCU->getTotalDistortion      () += uiBestDist;
    }

  } while (tuRecurseWithPU.nextSection(tuRecurseCU));

  //----- restore context models -----

  if( uiInitTrDepth != 0 )
  { // set Cbf for all blocks
    UInt uiCombCbfU = 0;
    UInt uiCombCbfV = 0;
    UInt uiPartIdx  = 0;
    for( UInt uiPart = 0; uiPart < 4; uiPart++, uiPartIdx += uiQNumParts )
    {
      uiCombCbfU |= pcCU->getCbf( uiPartIdx, COMPONENT_Cb, 1 );
      uiCombCbfV |= pcCU->getCbf( uiPartIdx, COMPONENT_Cr, 1 );
    }
    for( UInt uiOffs = 0; uiOffs < 4 * uiQNumParts; uiOffs++ )
    {
      pcCU->getCbf( COMPONENT_Cb )[ uiOffs ] |= uiCombCbfU;
      pcCU->getCbf( COMPONENT_Cr )[ uiOffs ] |= uiCombCbfV;
    }
  }

  m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[uiDepthCU][CI_CURR_BEST] );
}




/** Function for encoding and reconstructing luma/chroma samples of a PCM mode CU.
 * \param pcCU pointer to current CU
 * \param uiAbsPartIdx part index
 * \param pOrg pointer to original sample arrays
 * \param pPCM pointer to PCM code arrays
 * \param pPred pointer to prediction signal arrays
 * \param pResi pointer to residual signal arrays
 * \param pReco pointer to reconstructed sample arrays
 * \param uiStride stride of the original/prediction/residual sample arrays
 * \param uiWidth block width
 * \param uiHeight block height
 * \param compID texture component type
 */
Void TEncSearch::xEncPCM (TComDataCU* pcCU, UInt uiAbsPartIdx, Pel* pOrg, Pel* pPCM, Pel* pPred, Pel* pResi, Pel* pReco, UInt uiStride, UInt uiWidth, UInt uiHeight, const ComponentID compID )
{
  const UInt uiReconStride   = pcCU->getPic()->getPicYuvRec()->getStride(compID);
  const UInt uiPCMBitDepth   = pcCU->getSlice()->getSPS()->getPCMBitDepth(toChannelType(compID));
  const Int  channelBitDepth = pcCU->getSlice()->getSPS()->getBitDepth(toChannelType(compID));
  Pel* pRecoPic = pcCU->getPic()->getPicYuvRec()->getAddr(compID, pcCU->getCtuRsAddr(), pcCU->getZorderIdxInCtu()+uiAbsPartIdx);

  const Int pcmShiftRight=(channelBitDepth - Int(uiPCMBitDepth));

  assert(pcmShiftRight >= 0);

  for( UInt uiY = 0; uiY < uiHeight; uiY++ )
  {
    for( UInt uiX = 0; uiX < uiWidth; uiX++ )
    {
      // Reset pred and residual
      pPred[uiX] = 0;
      pResi[uiX] = 0;
      // Encode
      pPCM[uiX] = (pOrg[uiX]>>pcmShiftRight);
      // Reconstruction
      pReco   [uiX] = (pPCM[uiX]<<(pcmShiftRight));
      pRecoPic[uiX] = pReco[uiX];
    }
    pPred += uiStride;
    pResi += uiStride;
    pPCM += uiWidth;
    pOrg += uiStride;
    pReco += uiStride;
    pRecoPic += uiReconStride;
  }
}


//!  Function for PCM mode estimation.
Void TEncSearch::IPCMSearch( TComDataCU* pcCU, TComYuv* pcOrgYuv, TComYuv* pcPredYuv, TComYuv* pcResiYuv, TComYuv* pcRecoYuv )
{
  UInt              uiDepth      = pcCU->getDepth(0);
  const Distortion  uiDistortion = 0;
  UInt              uiBits;

  Double dCost;

  for (UInt ch=0; ch < pcCU->getPic()->getNumberValidComponents(); ch++)
  {
    const ComponentID compID  = ComponentID(ch);
    const UInt width  = pcCU->getWidth(0)  >> pcCU->getPic()->getComponentScaleX(compID);
    const UInt height = pcCU->getHeight(0) >> pcCU->getPic()->getComponentScaleY(compID);
    const UInt stride = pcPredYuv->getStride(compID);

    Pel * pOrig    = pcOrgYuv->getAddr  (compID, 0, width);
    Pel * pResi    = pcResiYuv->getAddr(compID, 0, width);
    Pel * pPred    = pcPredYuv->getAddr(compID, 0, width);
    Pel * pReco    = pcRecoYuv->getAddr(compID, 0, width);
    Pel * pPCM     = pcCU->getPCMSample (compID);

    xEncPCM ( pcCU, 0, pOrig, pPCM, pPred, pResi, pReco, stride, width, height, compID );

  }

  m_pcEntropyCoder->resetBits();
  xEncIntraHeader ( pcCU, uiDepth, 0, true, false);
  uiBits = m_pcEntropyCoder->getNumberOfWrittenBits();

  dCost = m_pcRdCost->calcRdCost( uiBits, uiDistortion );

  m_pcRDGoOnSbacCoder->load(m_pppcRDSbacCoder[uiDepth][CI_CURR_BEST]);

  pcCU->getTotalBits()       = uiBits;
  pcCU->getTotalCost()       = dCost;
  pcCU->getTotalDistortion() = uiDistortion;

  pcCU->copyToPic(uiDepth);
}




Void TEncSearch::xGetInterPredictionError( TComDataCU* pcCU, TComYuv* pcYuvOrg, Int iPartIdx, Distortion& ruiErr, Bool /*bHadamard*/ )
{
  motionCompensation( pcCU, &m_tmpYuvPred, REF_PIC_LIST_X, iPartIdx );

  UInt uiAbsPartIdx = 0;
  Int iWidth = 0;
  Int iHeight = 0;
  pcCU->getPartIndexAndSize( iPartIdx, uiAbsPartIdx, iWidth, iHeight );

  DistParam cDistParam;

  cDistParam.bApplyWeight = false;


  m_pcRdCost->setDistParam( cDistParam, pcCU->getSlice()->getSPS()->getBitDepth(CHANNEL_TYPE_LUMA),
                            pcYuvOrg->getAddr( COMPONENT_Y, uiAbsPartIdx ), pcYuvOrg->getStride(COMPONENT_Y),
                            m_tmpYuvPred .getAddr( COMPONENT_Y, uiAbsPartIdx ), m_tmpYuvPred.getStride(COMPONENT_Y),
                            iWidth, iHeight, m_pcEncCfg->getUseHADME() && (pcCU->getCUTransquantBypass(iPartIdx) == 0) );

  ruiErr = cDistParam.DistFunc( &cDistParam );
}

//! estimation of best merge coding
Void TEncSearch::xMergeEstimation( TComDataCU* pcCU, TComYuv* pcYuvOrg, Int iPUIdx, UInt& uiInterDir, TComMvField* pacMvField, UInt& uiMergeIndex, Distortion& ruiCost, TComMvField* cMvFieldNeighbours, UChar* uhInterDirNeighbours, Int& numValidMergeCand )
{

  UInt uiAbsPartIdx = 0;
  Int iWidth = 0;
  Int iHeight = 0;

 
  pcCU->getPartIndexAndSize( iPUIdx, uiAbsPartIdx, iWidth, iHeight );
  UInt uiDepth = pcCU->getDepth( uiAbsPartIdx );

  PartSize partSize = pcCU->getPartitionSize( 0 );
  if ( pcCU->getSlice()->getPPS()->getLog2ParallelMergeLevelMinus2() && partSize != SIZE_2Nx2N && pcCU->getWidth( 0 ) <= 8 )
  {
    if ( iPUIdx == 0 )
    {
      pcCU->setPartSizeSubParts( SIZE_2Nx2N, 0, uiDepth ); // temporarily set
      pcCU->getInterMergeCandidates( 0, 0, cMvFieldNeighbours,uhInterDirNeighbours, numValidMergeCand );
      pcCU->setPartSizeSubParts( partSize, 0, uiDepth ); // restore
    }
  }
  else
  {
    pcCU->getInterMergeCandidates( uiAbsPartIdx, iPUIdx, cMvFieldNeighbours, uhInterDirNeighbours, numValidMergeCand );
  }

  xRestrictBipredMergeCand( pcCU, iPUIdx, cMvFieldNeighbours, uhInterDirNeighbours, numValidMergeCand );

  ruiCost = std::numeric_limits<Distortion>::max();
  for( UInt uiMergeCand = 0; uiMergeCand < numValidMergeCand; ++uiMergeCand )
  {
    Distortion uiCostCand = std::numeric_limits<Distortion>::max();
    UInt       uiBitsCand = 0;

    PartSize ePartSize = pcCU->getPartitionSize( 0 );

    pcCU->getCUMvField(REF_PIC_LIST_0)->setAllMvField( cMvFieldNeighbours[0 + 2*uiMergeCand], ePartSize, uiAbsPartIdx, 0, iPUIdx );
    pcCU->getCUMvField(REF_PIC_LIST_1)->setAllMvField( cMvFieldNeighbours[1 + 2*uiMergeCand], ePartSize, uiAbsPartIdx, 0, iPUIdx );

    xGetInterPredictionError( pcCU, pcYuvOrg, iPUIdx, uiCostCand, m_pcEncCfg->getUseHADME() );
    uiBitsCand = uiMergeCand + 1;
    if (uiMergeCand == m_pcEncCfg->getMaxNumMergeCand() -1)
    {
        uiBitsCand--;
    }
    uiCostCand = uiCostCand + m_pcRdCost->getCost( uiBitsCand );
    if ( uiCostCand < ruiCost )
    {
      ruiCost = uiCostCand;
      pacMvField[0] = cMvFieldNeighbours[0 + 2*uiMergeCand];
      pacMvField[1] = cMvFieldNeighbours[1 + 2*uiMergeCand];
      uiInterDir = uhInterDirNeighbours[uiMergeCand];
      uiMergeIndex = uiMergeCand;
    }
  }
 
}

/** convert bi-pred merge candidates to uni-pred
 * \param pcCU
 * \param puIdx
 * \param mvFieldNeighbours
 * \param interDirNeighbours
 * \param numValidMergeCand
 * \returns Void
 */
Void TEncSearch::xRestrictBipredMergeCand( TComDataCU* pcCU, UInt puIdx, TComMvField* mvFieldNeighbours, UChar* interDirNeighbours, Int numValidMergeCand )
{
	
  if ( pcCU->isBipredRestriction(puIdx) )
  {
    for( UInt mergeCand = 0; mergeCand < numValidMergeCand; ++mergeCand )
    {
      if ( interDirNeighbours[mergeCand] == 3 )
      {
        interDirNeighbours[mergeCand] = 1;
        mvFieldNeighbours[(mergeCand << 1) + 1].setMvField(TComMv(0,0), -1);
      }
    }
  }
}

//! search of the best candidate for inter prediction
#if AMP_MRG
Void TEncSearch::predInterSearch( TComDataCU* pcCU, TComYuv* pcOrgYuv, TComYuv* pcPredYuv, TComYuv* pcResiYuv, TComYuv* pcRecoYuv DEBUG_STRING_FN_DECLARE(sDebug), Bool bUseRes, Bool bUseMRG )
#else
Void TEncSearch::predInterSearch( TComDataCU* pcCU, TComYuv* pcOrgYuv, TComYuv* pcPredYuv, TComYuv* pcResiYuv, TComYuv* pcRecoYuv, Bool bUseRes )
#endif
{
  for(UInt i=0; i<NUM_REF_PIC_LIST_01; i++)
  {
    m_acYuvPred[i].clear();
  }
  m_cYuvPredTemp.clear();
  pcPredYuv->clear();

  if ( !bUseRes )
  {
    pcResiYuv->clear();
  }

  pcRecoYuv->clear();
  
  TComMv       cMvSrchRngLT;
  TComMv       cMvSrchRngRB;

  TComMv       cMvZero;
  TComMv       TempMv; //kolya

  TComMv       cMv[2];
  TComMv       cMvBi[2];
  TComMv       cMvTemp[2][33];

  Int          iNumPart    = pcCU->getNumPartitions();
  Int          iNumPredDir = pcCU->getSlice()->isInterP() ? 1 : 2;

  TComMv       cMvPred[2][33];

  TComMv       cMvPredBi[2][33];
  Int          aaiMvpIdxBi[2][33];

  Int          aaiMvpIdx[2][33];
  Int          aaiMvpNum[2][33];

  AMVPInfo     aacAMVPInfo[2][33];

  Int          iRefIdx[2]={0,0}; //If un-initialized, may cause SEGV in bi-directional prediction iterative stage.
  Int          iRefIdxBi[2];

  UInt         uiPartAddr;
  Int          iRoiWidth, iRoiHeight;

  UInt         uiMbBits[3] = {1, 1, 0};

  UInt         uiLastMode = 0;
  Int          iRefStart, iRefEnd;

  PartSize     ePartSize = pcCU->getPartitionSize( 0 );

  Int          bestBiPRefIdxL1 = 0;
  Int          bestBiPMvpL1 = 0;
  Distortion   biPDistTemp = std::numeric_limits<Distortion>::max();

  TComMvField cMvFieldNeighbours[MRG_MAX_NUM_CANDS << 1]; // double length for mv of both lists
  UChar uhInterDirNeighbours[MRG_MAX_NUM_CANDS];
  Int numValidMergeCand = 0 ;

  for ( Int iPartIdx = 0; iPartIdx < iNumPart; iPartIdx++ )
  {
    Distortion   uiCost[2] = { std::numeric_limits<Distortion>::max(), std::numeric_limits<Distortion>::max() };
    Distortion   uiCostBi  =   std::numeric_limits<Distortion>::max();
    Distortion   uiCostTemp;

    UInt         uiBits[3];
    UInt         uiBitsTemp;
    Distortion   bestBiPDist = std::numeric_limits<Distortion>::max();

    Distortion   uiCostTempL0[MAX_NUM_REF];
    for (Int iNumRef=0; iNumRef < MAX_NUM_REF; iNumRef++)
    {
      uiCostTempL0[iNumRef] = std::numeric_limits<Distortion>::max();
    }
    UInt         uiBitsTempL0[MAX_NUM_REF];

    TComMv       mvValidList1;
    Int          refIdxValidList1 = 0;
    UInt         bitsValidList1 = MAX_UINT;
    Distortion   costValidList1 = std::numeric_limits<Distortion>::max();

    xGetBlkBits( ePartSize, pcCU->getSlice()->isInterP(), iPartIdx, uiLastMode, uiMbBits);

    pcCU->getPartIndexAndSize( iPartIdx, uiPartAddr, iRoiWidth, iRoiHeight );
	
	
#if AMP_MRG
    Bool bTestNormalMC = true;

    if ( bUseMRG && pcCU->getWidth( 0 ) > 8 && iNumPart == 2 )
    {
      bTestNormalMC = false;
    }

    if (bTestNormalMC)
    {
#endif

    //  Uni-directional prediction
    for ( Int iRefList = 0; iRefList < iNumPredDir; iRefList++ )
    {
      RefPicList  eRefPicList = ( iRefList ? REF_PIC_LIST_1 : REF_PIC_LIST_0 );

      for ( Int iRefIdxTemp = 0; iRefIdxTemp < pcCU->getSlice()->getNumRefIdx(eRefPicList); iRefIdxTemp++ )
      {
        uiBitsTemp = uiMbBits[iRefList];
        if ( pcCU->getSlice()->getNumRefIdx(eRefPicList) > 1 )
        {
          uiBitsTemp += iRefIdxTemp+1;
          if ( iRefIdxTemp == pcCU->getSlice()->getNumRefIdx(eRefPicList)-1 )
          {
            uiBitsTemp--;
          }
        }
        xEstimateMvPredAMVP( pcCU, pcOrgYuv, iPartIdx, eRefPicList, iRefIdxTemp, cMvPred[iRefList][iRefIdxTemp], false, &biPDistTemp);
        aaiMvpIdx[iRefList][iRefIdxTemp] = pcCU->getMVPIdx(eRefPicList, uiPartAddr);
        aaiMvpNum[iRefList][iRefIdxTemp] = pcCU->getMVPNum(eRefPicList, uiPartAddr);

        if(pcCU->getSlice()->getMvdL1ZeroFlag() && iRefList==1 && biPDistTemp < bestBiPDist)
        {
          bestBiPDist = biPDistTemp;
          bestBiPMvpL1 = aaiMvpIdx[iRefList][iRefIdxTemp];
          bestBiPRefIdxL1 = iRefIdxTemp;
        }

        uiBitsTemp += m_auiMVPIdxCost[aaiMvpIdx[iRefList][iRefIdxTemp]][AMVP_MAX_NUM_CANDS];

        if ( m_pcEncCfg->getFastMEForGenBLowDelayEnabled() && iRefList == 1 )    // list 1
        {
          if ( pcCU->getSlice()->getList1IdxToList0Idx( iRefIdxTemp ) >= 0 )
          {
            cMvTemp[1][iRefIdxTemp] = cMvTemp[0][pcCU->getSlice()->getList1IdxToList0Idx( iRefIdxTemp )];
            uiCostTemp = uiCostTempL0[pcCU->getSlice()->getList1IdxToList0Idx( iRefIdxTemp )];
            /*first subtract the bit-rate part of the cost of the other list*/
            uiCostTemp -= m_pcRdCost->getCost( uiBitsTempL0[pcCU->getSlice()->getList1IdxToList0Idx( iRefIdxTemp )] );
            /*correct the bit-rate part of the current ref*/
            m_pcRdCost->setPredictor  ( cMvPred[iRefList][iRefIdxTemp] );
            uiBitsTemp += m_pcRdCost->getBitsOfVectorWithPredictor( cMvTemp[1][iRefIdxTemp].getHor(), cMvTemp[1][iRefIdxTemp].getVer() );
            /*calculate the correct cost*/
            uiCostTemp += m_pcRdCost->getCost( uiBitsTemp );
          }
          else
          {
            xMotionEstimation ( pcCU, pcOrgYuv, iPartIdx, eRefPicList, &cMvPred[iRefList][iRefIdxTemp], iRefIdxTemp, cMvTemp[iRefList][iRefIdxTemp], uiBitsTemp, uiCostTemp );
          }
        }
        else
        {
          xMotionEstimation ( pcCU, pcOrgYuv, iPartIdx, eRefPicList, &cMvPred[iRefList][iRefIdxTemp], iRefIdxTemp, cMvTemp[iRefList][iRefIdxTemp], uiBitsTemp, uiCostTemp );
        }
        xCopyAMVPInfo(pcCU->getCUMvField(eRefPicList)->getAMVPInfo(), &aacAMVPInfo[iRefList][iRefIdxTemp]); // must always be done ( also when AMVP_MODE = AM_NONE )
        xCheckBestMVP(pcCU, eRefPicList, cMvTemp[iRefList][iRefIdxTemp], cMvPred[iRefList][iRefIdxTemp], aaiMvpIdx[iRefList][iRefIdxTemp], uiBitsTemp, uiCostTemp);

        if ( iRefList == 0 )
        {
          uiCostTempL0[iRefIdxTemp] = uiCostTemp;
          uiBitsTempL0[iRefIdxTemp] = uiBitsTemp;
        }
        if ( uiCostTemp < uiCost[iRefList] )
        {
          uiCost[iRefList] = uiCostTemp;
          uiBits[iRefList] = uiBitsTemp; // storing for bi-prediction

          // set motion
          cMv[iRefList]     = cMvTemp[iRefList][iRefIdxTemp];
          iRefIdx[iRefList] = iRefIdxTemp;
        }

        if ( iRefList == 1 && uiCostTemp < costValidList1 && pcCU->getSlice()->getList1IdxToList0Idx( iRefIdxTemp ) < 0 )
        {
          costValidList1 = uiCostTemp;
          bitsValidList1 = uiBitsTemp;

          // set motion
          mvValidList1     = cMvTemp[iRefList][iRefIdxTemp];
          refIdxValidList1 = iRefIdxTemp;
        }
      }
    }

    //  Bi-predictive Motion estimation
    if ( (pcCU->getSlice()->isInterB()) && (pcCU->isBipredRestriction(iPartIdx) == false) )
    {

      cMvBi[0] = cMv[0];            cMvBi[1] = cMv[1];
      iRefIdxBi[0] = iRefIdx[0];    iRefIdxBi[1] = iRefIdx[1];

      ::memcpy(cMvPredBi, cMvPred, sizeof(cMvPred));
      ::memcpy(aaiMvpIdxBi, aaiMvpIdx, sizeof(aaiMvpIdx));

      UInt uiMotBits[2];

      if(pcCU->getSlice()->getMvdL1ZeroFlag())
      {
        xCopyAMVPInfo(&aacAMVPInfo[1][bestBiPRefIdxL1], pcCU->getCUMvField(REF_PIC_LIST_1)->getAMVPInfo());
        pcCU->setMVPIdxSubParts( bestBiPMvpL1, REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
        aaiMvpIdxBi[1][bestBiPRefIdxL1] = bestBiPMvpL1;
        cMvPredBi[1][bestBiPRefIdxL1]   = pcCU->getCUMvField(REF_PIC_LIST_1)->getAMVPInfo()->m_acMvCand[bestBiPMvpL1];

        cMvBi[1] = cMvPredBi[1][bestBiPRefIdxL1];
        iRefIdxBi[1] = bestBiPRefIdxL1;
        pcCU->getCUMvField( REF_PIC_LIST_1 )->setAllMv( cMvBi[1], ePartSize, uiPartAddr, 0, iPartIdx );
        pcCU->getCUMvField( REF_PIC_LIST_1 )->setAllRefIdx( iRefIdxBi[1], ePartSize, uiPartAddr, 0, iPartIdx );
        TComYuv* pcYuvPred = &m_acYuvPred[REF_PIC_LIST_1];
        motionCompensation( pcCU, pcYuvPred, REF_PIC_LIST_1, iPartIdx );

        uiMotBits[0] = uiBits[0] - uiMbBits[0];
        uiMotBits[1] = uiMbBits[1];

        if ( pcCU->getSlice()->getNumRefIdx(REF_PIC_LIST_1) > 1 )
        {
          uiMotBits[1] += bestBiPRefIdxL1+1;
          if ( bestBiPRefIdxL1 == pcCU->getSlice()->getNumRefIdx(REF_PIC_LIST_1)-1 )
          {
            uiMotBits[1]--;
          }
        }

        uiMotBits[1] += m_auiMVPIdxCost[aaiMvpIdxBi[1][bestBiPRefIdxL1]][AMVP_MAX_NUM_CANDS];

        uiBits[2] = uiMbBits[2] + uiMotBits[0] + uiMotBits[1];

        cMvTemp[1][bestBiPRefIdxL1] = cMvBi[1];
      }
      else
      {
        uiMotBits[0] = uiBits[0] - uiMbBits[0];
        uiMotBits[1] = uiBits[1] - uiMbBits[1];
        uiBits[2] = uiMbBits[2] + uiMotBits[0] + uiMotBits[1];
      }

      // 4-times iteration (default)
      Int iNumIter = 4;

      // fast encoder setting: only one iteration
      if ( m_pcEncCfg->getFastInterSearchMode()==FASTINTERSEARCH_MODE1 || m_pcEncCfg->getFastInterSearchMode()==FASTINTERSEARCH_MODE2 || pcCU->getSlice()->getMvdL1ZeroFlag() )
      {
        iNumIter = 1;
      }

      for ( Int iIter = 0; iIter < iNumIter; iIter++ )
      {
        Int         iRefList    = iIter % 2;

        if ( m_pcEncCfg->getFastInterSearchMode()==FASTINTERSEARCH_MODE1 || m_pcEncCfg->getFastInterSearchMode()==FASTINTERSEARCH_MODE2 )
        {
          if( uiCost[0] <= uiCost[1] )
          {
            iRefList = 1;
          }
          else
          {
            iRefList = 0;
          }
        }
        else if ( iIter == 0 )
        {
          iRefList = 0;
        }
        if ( iIter == 0 && !pcCU->getSlice()->getMvdL1ZeroFlag())
        {
          pcCU->getCUMvField(RefPicList(1-iRefList))->setAllMv( cMv[1-iRefList], ePartSize, uiPartAddr, 0, iPartIdx );
          pcCU->getCUMvField(RefPicList(1-iRefList))->setAllRefIdx( iRefIdx[1-iRefList], ePartSize, uiPartAddr, 0, iPartIdx );
          TComYuv*  pcYuvPred = &m_acYuvPred[1-iRefList];
          motionCompensation ( pcCU, pcYuvPred, RefPicList(1-iRefList), iPartIdx );
        }

        RefPicList  eRefPicList = ( iRefList ? REF_PIC_LIST_1 : REF_PIC_LIST_0 );

        if(pcCU->getSlice()->getMvdL1ZeroFlag())
        {
          iRefList = 0;
          eRefPicList = REF_PIC_LIST_0;
        }

        Bool bChanged = false;

        iRefStart = 0;
        iRefEnd   = pcCU->getSlice()->getNumRefIdx(eRefPicList)-1;

        for ( Int iRefIdxTemp = iRefStart; iRefIdxTemp <= iRefEnd; iRefIdxTemp++ )
        {
          uiBitsTemp = uiMbBits[2] + uiMotBits[1-iRefList];
          if ( pcCU->getSlice()->getNumRefIdx(eRefPicList) > 1 )
          {
            uiBitsTemp += iRefIdxTemp+1;
            if ( iRefIdxTemp == pcCU->getSlice()->getNumRefIdx(eRefPicList)-1 )
            {
              uiBitsTemp--;
            }
          }
          uiBitsTemp += m_auiMVPIdxCost[aaiMvpIdxBi[iRefList][iRefIdxTemp]][AMVP_MAX_NUM_CANDS];
          // call ME
          xMotionEstimation ( pcCU, pcOrgYuv, iPartIdx, eRefPicList, &cMvPredBi[iRefList][iRefIdxTemp], iRefIdxTemp, cMvTemp[iRefList][iRefIdxTemp], uiBitsTemp, uiCostTemp, true );

          xCopyAMVPInfo(&aacAMVPInfo[iRefList][iRefIdxTemp], pcCU->getCUMvField(eRefPicList)->getAMVPInfo());
          xCheckBestMVP(pcCU, eRefPicList, cMvTemp[iRefList][iRefIdxTemp], cMvPredBi[iRefList][iRefIdxTemp], aaiMvpIdxBi[iRefList][iRefIdxTemp], uiBitsTemp, uiCostTemp);

          if ( uiCostTemp < uiCostBi )
          {
            bChanged = true;

            cMvBi[iRefList]     = cMvTemp[iRefList][iRefIdxTemp];
            iRefIdxBi[iRefList] = iRefIdxTemp;

            uiCostBi            = uiCostTemp;
            uiMotBits[iRefList] = uiBitsTemp - uiMbBits[2] - uiMotBits[1-iRefList];
            uiBits[2]           = uiBitsTemp;

            if(iNumIter!=1)
            {
              //  Set motion
              pcCU->getCUMvField( eRefPicList )->setAllMv( cMvBi[iRefList], ePartSize, uiPartAddr, 0, iPartIdx );
              pcCU->getCUMvField( eRefPicList )->setAllRefIdx( iRefIdxBi[iRefList], ePartSize, uiPartAddr, 0, iPartIdx );

              TComYuv* pcYuvPred = &m_acYuvPred[iRefList];
              motionCompensation( pcCU, pcYuvPred, eRefPicList, iPartIdx );
            }
          }
        } // for loop-iRefIdxTemp

        if ( !bChanged )
        {
          if ( uiCostBi <= uiCost[0] && uiCostBi <= uiCost[1] )
          {
            xCopyAMVPInfo(&aacAMVPInfo[0][iRefIdxBi[0]], pcCU->getCUMvField(REF_PIC_LIST_0)->getAMVPInfo());
            xCheckBestMVP(pcCU, REF_PIC_LIST_0, cMvBi[0], cMvPredBi[0][iRefIdxBi[0]], aaiMvpIdxBi[0][iRefIdxBi[0]], uiBits[2], uiCostBi);
            if(!pcCU->getSlice()->getMvdL1ZeroFlag())
            {
              xCopyAMVPInfo(&aacAMVPInfo[1][iRefIdxBi[1]], pcCU->getCUMvField(REF_PIC_LIST_1)->getAMVPInfo());
              xCheckBestMVP(pcCU, REF_PIC_LIST_1, cMvBi[1], cMvPredBi[1][iRefIdxBi[1]], aaiMvpIdxBi[1][iRefIdxBi[1]], uiBits[2], uiCostBi);
            }
          }
          break;
        }
      } // for loop-iter
    } // if (B_SLICE)

#if AMP_MRG
    } //end if bTestNormalMC
#endif
    //  Clear Motion Field
    pcCU->getCUMvField(REF_PIC_LIST_0)->setAllMvField( TComMvField(), ePartSize, uiPartAddr, 0, iPartIdx );
    pcCU->getCUMvField(REF_PIC_LIST_1)->setAllMvField( TComMvField(), ePartSize, uiPartAddr, 0, iPartIdx );
    pcCU->getCUMvField(REF_PIC_LIST_0)->setAllMvd    ( cMvZero,       ePartSize, uiPartAddr, 0, iPartIdx );
    pcCU->getCUMvField(REF_PIC_LIST_1)->setAllMvd    ( cMvZero,       ePartSize, uiPartAddr, 0, iPartIdx );

    pcCU->setMVPIdxSubParts( -1, REF_PIC_LIST_0, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
    pcCU->setMVPNumSubParts( -1, REF_PIC_LIST_0, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
    pcCU->setMVPIdxSubParts( -1, REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
    pcCU->setMVPNumSubParts( -1, REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));

    UInt uiMEBits = 0;
    // Set Motion Field_
    cMv[1] = mvValidList1;
	
    iRefIdx[1] = refIdxValidList1;
    uiBits[1] = bitsValidList1;
    uiCost[1] = costValidList1;

#if AMP_MRG
    if (bTestNormalMC)
    {
#endif
    if ( uiCostBi <= uiCost[0] && uiCostBi <= uiCost[1])
    {
      uiLastMode = 2;
      pcCU->getCUMvField(REF_PIC_LIST_0)->setAllMv( cMvBi[0], ePartSize, uiPartAddr, 0, iPartIdx );
      pcCU->getCUMvField(REF_PIC_LIST_0)->setAllRefIdx( iRefIdxBi[0], ePartSize, uiPartAddr, 0, iPartIdx );
      pcCU->getCUMvField(REF_PIC_LIST_1)->setAllMv( cMvBi[1], ePartSize, uiPartAddr, 0, iPartIdx );
      pcCU->getCUMvField(REF_PIC_LIST_1)->setAllRefIdx( iRefIdxBi[1], ePartSize, uiPartAddr, 0, iPartIdx );

      TempMv = cMvBi[0] - cMvPredBi[0][iRefIdxBi[0]];
      pcCU->getCUMvField(REF_PIC_LIST_0)->setAllMvd    ( TempMv,                 ePartSize, uiPartAddr, 0, iPartIdx );

      TempMv = cMvBi[1] - cMvPredBi[1][iRefIdxBi[1]];
      pcCU->getCUMvField(REF_PIC_LIST_1)->setAllMvd    ( TempMv,                 ePartSize, uiPartAddr, 0, iPartIdx );

      pcCU->setInterDirSubParts( 3, uiPartAddr, iPartIdx, pcCU->getDepth(0) );

      pcCU->setMVPIdxSubParts( aaiMvpIdxBi[0][iRefIdxBi[0]], REF_PIC_LIST_0, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
      pcCU->setMVPNumSubParts( aaiMvpNum[0][iRefIdxBi[0]], REF_PIC_LIST_0, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
      pcCU->setMVPIdxSubParts( aaiMvpIdxBi[1][iRefIdxBi[1]], REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
      pcCU->setMVPNumSubParts( aaiMvpNum[1][iRefIdxBi[1]], REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));

      uiMEBits = uiBits[2];
    }
    else if ( uiCost[0] <= uiCost[1] )
    {
      uiLastMode = 0;
      pcCU->getCUMvField(REF_PIC_LIST_0)->setAllMv( cMv[0], ePartSize, uiPartAddr, 0, iPartIdx );
      pcCU->getCUMvField(REF_PIC_LIST_0)->setAllRefIdx( iRefIdx[0], ePartSize, uiPartAddr, 0, iPartIdx );

      TempMv = cMv[0] - cMvPred[0][iRefIdx[0]];
      pcCU->getCUMvField(REF_PIC_LIST_0)->setAllMvd    ( TempMv,                 ePartSize, uiPartAddr, 0, iPartIdx );

      pcCU->setInterDirSubParts( 1, uiPartAddr, iPartIdx, pcCU->getDepth(0) );

      pcCU->setMVPIdxSubParts( aaiMvpIdx[0][iRefIdx[0]], REF_PIC_LIST_0, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
      pcCU->setMVPNumSubParts( aaiMvpNum[0][iRefIdx[0]], REF_PIC_LIST_0, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));

      uiMEBits = uiBits[0];
    }
    else
    {
      uiLastMode = 1;
      pcCU->getCUMvField(REF_PIC_LIST_1)->setAllMv( cMv[1], ePartSize, uiPartAddr, 0, iPartIdx );
      pcCU->getCUMvField(REF_PIC_LIST_1)->setAllRefIdx( iRefIdx[1], ePartSize, uiPartAddr, 0, iPartIdx );

      TempMv = cMv[1] - cMvPred[1][iRefIdx[1]];
      pcCU->getCUMvField(REF_PIC_LIST_1)->setAllMvd    ( TempMv,                 ePartSize, uiPartAddr, 0, iPartIdx );

      pcCU->setInterDirSubParts( 2, uiPartAddr, iPartIdx, pcCU->getDepth(0) );

      pcCU->setMVPIdxSubParts( aaiMvpIdx[1][iRefIdx[1]], REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
      pcCU->setMVPNumSubParts( aaiMvpNum[1][iRefIdx[1]], REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));

      uiMEBits = uiBits[1];
    }
#if AMP_MRG
    } // end if bTestNormalMC
#endif

    if ( pcCU->getPartitionSize( uiPartAddr ) != SIZE_2Nx2N )
    {
      UInt uiMRGInterDir = 0;
      TComMvField cMRGMvField[2];
      UInt uiMRGIndex = 0;

      UInt uiMEInterDir = 0;
      TComMvField cMEMvField[2];

      m_pcRdCost->selectMotionLambda( true, 0, pcCU->getCUTransquantBypass(uiPartAddr) );

#if AMP_MRG
      // calculate ME cost
      Distortion uiMEError = std::numeric_limits<Distortion>::max();
      Distortion uiMECost  = std::numeric_limits<Distortion>::max();

      if (bTestNormalMC)
      {
        xGetInterPredictionError( pcCU, pcOrgYuv, iPartIdx, uiMEError, m_pcEncCfg->getUseHADME() );
        uiMECost = uiMEError + m_pcRdCost->getCost( uiMEBits );
      }
#else
      // calculate ME cost
      Distortion uiMEError = std::numeric_limits<Distortion>::max();
      xGetInterPredictionError( pcCU, pcOrgYuv, iPartIdx, uiMEError, m_pcEncCfg->getUseHADME() );
      Distortion uiMECost = uiMEError + m_pcRdCost->getCost( uiMEBits );
#endif
      // save ME result.
      uiMEInterDir = pcCU->getInterDir( uiPartAddr );
      TComDataCU::getMvField( pcCU, uiPartAddr, REF_PIC_LIST_0, cMEMvField[0] );
      TComDataCU::getMvField( pcCU, uiPartAddr, REF_PIC_LIST_1, cMEMvField[1] );

      // find Merge result
      Distortion uiMRGCost = std::numeric_limits<Distortion>::max();

      xMergeEstimation( pcCU, pcOrgYuv, iPartIdx, uiMRGInterDir, cMRGMvField, uiMRGIndex, uiMRGCost, cMvFieldNeighbours, uhInterDirNeighbours, numValidMergeCand);

      if ( uiMRGCost < uiMECost )
      {
        // set Merge result
        pcCU->setMergeFlagSubParts ( true,          uiPartAddr, iPartIdx, pcCU->getDepth( uiPartAddr ) );
        pcCU->setMergeIndexSubParts( uiMRGIndex,    uiPartAddr, iPartIdx, pcCU->getDepth( uiPartAddr ) );
        pcCU->setInterDirSubParts  ( uiMRGInterDir, uiPartAddr, iPartIdx, pcCU->getDepth( uiPartAddr ) );
        pcCU->getCUMvField( REF_PIC_LIST_0 )->setAllMvField( cMRGMvField[0], ePartSize, uiPartAddr, 0, iPartIdx );
        pcCU->getCUMvField( REF_PIC_LIST_1 )->setAllMvField( cMRGMvField[1], ePartSize, uiPartAddr, 0, iPartIdx );

        pcCU->getCUMvField(REF_PIC_LIST_0)->setAllMvd    ( cMvZero,            ePartSize, uiPartAddr, 0, iPartIdx );
        pcCU->getCUMvField(REF_PIC_LIST_1)->setAllMvd    ( cMvZero,            ePartSize, uiPartAddr, 0, iPartIdx );

        pcCU->setMVPIdxSubParts( -1, REF_PIC_LIST_0, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
        pcCU->setMVPNumSubParts( -1, REF_PIC_LIST_0, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
        pcCU->setMVPIdxSubParts( -1, REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
        pcCU->setMVPNumSubParts( -1, REF_PIC_LIST_1, uiPartAddr, iPartIdx, pcCU->getDepth(uiPartAddr));
      }
      else
      {
        // set ME result
        pcCU->setMergeFlagSubParts( false,        uiPartAddr, iPartIdx, pcCU->getDepth( uiPartAddr ) );
        pcCU->setInterDirSubParts ( uiMEInterDir, uiPartAddr, iPartIdx, pcCU->getDepth( uiPartAddr ) );
        pcCU->getCUMvField( REF_PIC_LIST_0 )->setAllMvField( cMEMvField[0], ePartSize, uiPartAddr, 0, iPartIdx );
        pcCU->getCUMvField( REF_PIC_LIST_1 )->setAllMvField( cMEMvField[1], ePartSize, uiPartAddr, 0, iPartIdx );
      }
    }

    //  MC
    motionCompensation ( pcCU, pcPredYuv, REF_PIC_LIST_X, iPartIdx );

  } //  end of for ( Int iPartIdx = 0; iPartIdx < iNumPart; iPartIdx++ )

  setWpScalingDistParam( pcCU, -1, REF_PIC_LIST_X );
  
  return;
}


// AMVP
Void TEncSearch::xEstimateMvPredAMVP( TComDataCU* pcCU, TComYuv* pcOrgYuv, UInt uiPartIdx, RefPicList eRefPicList, Int iRefIdx, TComMv& rcMvPred, Bool bFilled, Distortion* puiDistBiP )
{

  AMVPInfo*  pcAMVPInfo = pcCU->getCUMvField(eRefPicList)->getAMVPInfo();

  TComMv     cBestMv;
  Int        iBestIdx   = 0;
  TComMv     cZeroMv;
  TComMv     cMvPred;
  Distortion uiBestCost = std::numeric_limits<Distortion>::max();
  UInt       uiPartAddr = 0;
  Int        iRoiWidth, iRoiHeight;
  Int        i;

  pcCU->getPartIndexAndSize( uiPartIdx, uiPartAddr, iRoiWidth, iRoiHeight );


  // Fill the MV Candidates
  if (!bFilled)
  {
    pcCU->fillMvpCand( uiPartIdx, uiPartAddr, eRefPicList, iRefIdx, pcAMVPInfo );
  }

  // initialize Mvp index & Mvp
  iBestIdx = 0;
  cBestMv  = pcAMVPInfo->m_acMvCand[0];
  if (pcAMVPInfo->iN <= 1)
  {
    rcMvPred = cBestMv;

    pcCU->setMVPIdxSubParts( iBestIdx, eRefPicList, uiPartAddr, uiPartIdx, pcCU->getDepth(uiPartAddr));
    pcCU->setMVPNumSubParts( pcAMVPInfo->iN, eRefPicList, uiPartAddr, uiPartIdx, pcCU->getDepth(uiPartAddr));

    if(pcCU->getSlice()->getMvdL1ZeroFlag() && eRefPicList==REF_PIC_LIST_1)
    {
      (*puiDistBiP) = xGetTemplateCost( pcCU, uiPartAddr, pcOrgYuv, &m_cYuvPredTemp, rcMvPred, 0, AMVP_MAX_NUM_CANDS, eRefPicList, iRefIdx, iRoiWidth, iRoiHeight);
    }
    return;
  }

  if (bFilled)
  {
    assert(pcCU->getMVPIdx(eRefPicList,uiPartAddr) >= 0);
    rcMvPred = pcAMVPInfo->m_acMvCand[pcCU->getMVPIdx(eRefPicList,uiPartAddr)];
    return;
  }

  m_cYuvPredTemp.clear();
  //-- Check Minimum Cost.
  for ( i = 0 ; i < pcAMVPInfo->iN; i++)
  {
    Distortion uiTmpCost;
    uiTmpCost = xGetTemplateCost( pcCU, uiPartAddr, pcOrgYuv, &m_cYuvPredTemp, pcAMVPInfo->m_acMvCand[i], i, AMVP_MAX_NUM_CANDS, eRefPicList, iRefIdx, iRoiWidth, iRoiHeight);
    if ( uiBestCost > uiTmpCost )
    {
      uiBestCost = uiTmpCost;
      cBestMv   = pcAMVPInfo->m_acMvCand[i];
      iBestIdx  = i;
      (*puiDistBiP) = uiTmpCost;
    }
  }

  m_cYuvPredTemp.clear();

  // Setting Best MVP
  rcMvPred = cBestMv;
  pcCU->setMVPIdxSubParts( iBestIdx, eRefPicList, uiPartAddr, uiPartIdx, pcCU->getDepth(uiPartAddr));
  pcCU->setMVPNumSubParts( pcAMVPInfo->iN, eRefPicList, uiPartAddr, uiPartIdx, pcCU->getDepth(uiPartAddr));
  return;
  
}

UInt TEncSearch::xGetMvpIdxBits(Int iIdx, Int iNum)
{
  assert(iIdx >= 0 && iNum >= 0 && iIdx < iNum);

  if (iNum == 1)
  {
    return 0;
  }

  UInt uiLength = 1;
  Int iTemp = iIdx;
  if ( iTemp == 0 )
  {
    return uiLength;
  }

  Bool bCodeLast = ( iNum-1 > iTemp );

  uiLength += (iTemp-1);

  if( bCodeLast )
  {
    uiLength++;
  }

  return uiLength;
}

Void TEncSearch::xGetBlkBits( PartSize eCUMode, Bool bPSlice, Int iPartIdx, UInt uiLastMode, UInt uiBlkBit[3])
{
  if ( eCUMode == SIZE_2Nx2N )
  {
    uiBlkBit[0] = (! bPSlice) ? 3 : 1;
    uiBlkBit[1] = 3;
    uiBlkBit[2] = 5;
  }
  else if ( (eCUMode == SIZE_2NxN || eCUMode == SIZE_2NxnU) || eCUMode == SIZE_2NxnD )
  {
    UInt aauiMbBits[2][3][3] = { { {0,0,3}, {0,0,0}, {0,0,0} } , { {5,7,7}, {7,5,7}, {9-3,9-3,9-3} } };
    if ( bPSlice )
    {
      uiBlkBit[0] = 3;
      uiBlkBit[1] = 0;
      uiBlkBit[2] = 0;
    }
    else
    {
      ::memcpy( uiBlkBit, aauiMbBits[iPartIdx][uiLastMode], 3*sizeof(UInt) );
    }
  }
  else if ( (eCUMode == SIZE_Nx2N || eCUMode == SIZE_nLx2N) || eCUMode == SIZE_nRx2N )
  {
    UInt aauiMbBits[2][3][3] = { { {0,2,3}, {0,0,0}, {0,0,0} } , { {5,7,7}, {7-2,7-2,9-2}, {9-3,9-3,9-3} } };
    if ( bPSlice )
    {
      uiBlkBit[0] = 3;
      uiBlkBit[1] = 0;
      uiBlkBit[2] = 0;
    }
    else
    {
      ::memcpy( uiBlkBit, aauiMbBits[iPartIdx][uiLastMode], 3*sizeof(UInt) );
    }
  }
  else if ( eCUMode == SIZE_NxN )
  {
    uiBlkBit[0] = (! bPSlice) ? 3 : 1;
    uiBlkBit[1] = 3;
    uiBlkBit[2] = 5;
  }
  else
  {
    printf("Wrong!\n");
    assert( 0 );
  }
}

Void TEncSearch::xCopyAMVPInfo (AMVPInfo* pSrc, AMVPInfo* pDst)
{
  pDst->iN = pSrc->iN;
  for (Int i = 0; i < pSrc->iN; i++)
  {
    pDst->m_acMvCand[i] = pSrc->m_acMvCand[i];
  }
}

Void TEncSearch::xCheckBestMVP ( TComDataCU* pcCU, RefPicList eRefPicList, TComMv cMv, TComMv& rcMvPred, Int& riMVPIdx, UInt& ruiBits, Distortion& ruiCost )
{
  AMVPInfo* pcAMVPInfo = pcCU->getCUMvField(eRefPicList)->getAMVPInfo();
  
  assert(pcAMVPInfo->m_acMvCand[riMVPIdx] == rcMvPred);

  if (pcAMVPInfo->iN < 2)
  {
    return;
  }

  m_pcRdCost->selectMotionLambda( true, 0, pcCU->getCUTransquantBypass(0) );
  m_pcRdCost->setCostScale ( 0    );

  Int iBestMVPIdx = riMVPIdx;

  m_pcRdCost->setPredictor( rcMvPred );
  Int iOrgMvBits  = m_pcRdCost->getBitsOfVectorWithPredictor(cMv.getHor(), cMv.getVer());
  iOrgMvBits += m_auiMVPIdxCost[riMVPIdx][AMVP_MAX_NUM_CANDS];
  Int iBestMvBits = iOrgMvBits;

  for (Int iMVPIdx = 0; iMVPIdx < pcAMVPInfo->iN; iMVPIdx++)
  {
    if (iMVPIdx == riMVPIdx)
    {
      continue;
    }

    m_pcRdCost->setPredictor( pcAMVPInfo->m_acMvCand[iMVPIdx] );

    Int iMvBits = m_pcRdCost->getBitsOfVectorWithPredictor(cMv.getHor(), cMv.getVer());
    iMvBits += m_auiMVPIdxCost[iMVPIdx][AMVP_MAX_NUM_CANDS];

    if (iMvBits < iBestMvBits)
    {
      iBestMvBits = iMvBits;
      iBestMVPIdx = iMVPIdx;
    }
  }

  if (iBestMVPIdx != riMVPIdx)  //if changed
  {
    rcMvPred = pcAMVPInfo->m_acMvCand[iBestMVPIdx];

    riMVPIdx = iBestMVPIdx;
    UInt uiOrgBits = ruiBits;
    ruiBits = uiOrgBits - iOrgMvBits + iBestMvBits;
    ruiCost = (ruiCost - m_pcRdCost->getCost( uiOrgBits ))  + m_pcRdCost->getCost( ruiBits );
  }
  
}


Distortion TEncSearch::xGetTemplateCost( TComDataCU* pcCU,
                                         UInt        uiPartAddr,
                                         TComYuv*    pcOrgYuv,
                                         TComYuv*    pcTemplateCand,
                                         TComMv      cMvCand,
                                         Int         iMVPIdx,
                                         Int         iMVPNum,
                                         RefPicList  eRefPicList,
                                         Int         iRefIdx,
                                         Int         iSizeX,
                                         Int         iSizeY
                                         )
{
  Distortion uiCost = std::numeric_limits<Distortion>::max();

  TComPicYuv* pcPicYuvRef = pcCU->getSlice()->getRefPic( eRefPicList, iRefIdx )->getPicYuvRec();

  pcCU->clipMv( cMvCand );

  // prediction pattern
  if ( pcCU->getSlice()->testWeightPred() && pcCU->getSlice()->getSliceType()==P_SLICE )
  {
    xPredInterBlk( COMPONENT_Y, pcCU, pcPicYuvRef, uiPartAddr, &cMvCand, iSizeX, iSizeY, pcTemplateCand, true, pcCU->getSlice()->getSPS()->getBitDepth(CHANNEL_TYPE_LUMA) );
  }
  else
  {
    xPredInterBlk( COMPONENT_Y, pcCU, pcPicYuvRef, uiPartAddr, &cMvCand, iSizeX, iSizeY, pcTemplateCand, false, pcCU->getSlice()->getSPS()->getBitDepth(CHANNEL_TYPE_LUMA) );
  }

  if ( pcCU->getSlice()->testWeightPred() && pcCU->getSlice()->getSliceType()==P_SLICE )
  {
    xWeightedPredictionUni( pcCU, pcTemplateCand, uiPartAddr, iSizeX, iSizeY, eRefPicList, pcTemplateCand, iRefIdx );
  }

  // calc distortion

  uiCost = m_pcRdCost->getDistPart( pcCU->getSlice()->getSPS()->getBitDepth(CHANNEL_TYPE_LUMA), pcTemplateCand->getAddr(COMPONENT_Y, uiPartAddr), pcTemplateCand->getStride(COMPONENT_Y), pcOrgYuv->getAddr(COMPONENT_Y, uiPartAddr), pcOrgYuv->getStride(COMPONENT_Y), iSizeX, iSizeY, COMPONENT_Y, DF_SAD );
  uiCost = (UInt) m_pcRdCost->calcRdCost( m_auiMVPIdxCost[iMVPIdx][iMVPNum], uiCost, DF_SAD );
  return uiCost;
}


Void TEncSearch::xMotionEstimation( TComDataCU* pcCU, TComYuv* pcYuvOrg, Int iPartIdx, RefPicList eRefPicList, TComMv* pcMvPred, Int iRefIdxPred, TComMv& rcMv, UInt& ruiBits, Distortion& ruiCost, Bool bBi  )
{
  UInt          uiPartAddr;
  Int           iRoiWidth;
  Int           iRoiHeight;

  TComMv        cMvHalf, cMvQter;
  TComMv        cMvSrchRngLT;
  TComMv        cMvSrchRngRB;
  TComYuv*      pcYuv = pcYuvOrg;

  assert(eRefPicList < MAX_NUM_REF_LIST_ADAPT_SR && iRefIdxPred<Int(MAX_IDX_ADAPT_SR));
  m_iSearchRange = m_aaiAdaptSR[eRefPicList][iRefIdxPred];

  Int           iSrchRng      = ( bBi ? m_bipredSearchRange : m_iSearchRange );
  TComPattern   tmpPattern;
  TComPattern*  pcPatternKey  = &tmpPattern;

  Double        fWeight       = 1.0;

  pcCU->getPartIndexAndSize( iPartIdx, uiPartAddr, iRoiWidth, iRoiHeight );

  if ( bBi ) // Bipredictive ME
  {
    TComYuv*  pcYuvOther = &m_acYuvPred[1-(Int)eRefPicList];
    pcYuv                = &m_cYuvPredTemp;

    pcYuvOrg->copyPartToPartYuv( pcYuv, uiPartAddr, iRoiWidth, iRoiHeight );

    pcYuv->removeHighFreq( pcYuvOther, uiPartAddr, iRoiWidth, iRoiHeight, pcCU->getSlice()->getSPS()->getBitDepths().recon, m_pcEncCfg->getClipForBiPredMeEnabled() );

    fWeight = 0.5;
  }
  m_cDistParam.bIsBiPred = bBi;

  //  Search key pattern initialization
  pcPatternKey->initPattern( pcYuv->getAddr  ( COMPONENT_Y, uiPartAddr ),
                             iRoiWidth,
                             iRoiHeight,
                             pcYuv->getStride(COMPONENT_Y),
                             pcCU->getSlice()->getSPS()->getBitDepth(CHANNEL_TYPE_LUMA) );

  Pel*        piRefY      = pcCU->getSlice()->getRefPic( eRefPicList, iRefIdxPred )->getPicYuvRec()->getAddr( COMPONENT_Y, pcCU->getCtuRsAddr(), pcCU->getZorderIdxInCtu() + uiPartAddr );
  Int         iRefStride  = pcCU->getSlice()->getRefPic( eRefPicList, iRefIdxPred )->getPicYuvRec()->getStride(COMPONENT_Y);

  TComMv      cMvPred = *pcMvPred;

  if ( bBi )
  {
	  
    xSetSearchRange   ( pcCU, rcMv   , iSrchRng, cMvSrchRngLT, cMvSrchRngRB );
  }
  else
  {
	  
    xSetSearchRange   ( pcCU, cMvPred, iSrchRng, cMvSrchRngLT, cMvSrchRngRB );
  }

  m_pcRdCost->selectMotionLambda(true, 0, pcCU->getCUTransquantBypass(uiPartAddr) );

  m_pcRdCost->setPredictor  ( *pcMvPred );
  m_pcRdCost->setCostScale  ( 2 );

  setWpScalingDistParam( pcCU, iRefIdxPred, eRefPicList );
  //  Do integer search
  if ( (m_motionEstimationSearchMethod==MESEARCH_FULL) || bBi )
  {
    xPatternSearch      ( pcPatternKey, piRefY, iRefStride, &cMvSrchRngLT, &cMvSrchRngRB, rcMv, ruiCost );
  }
  else
  {
    rcMv = *pcMvPred;
    const TComMv *pIntegerMv2Nx2NPred=0;
    if (pcCU->getPartitionSize(0) != SIZE_2Nx2N || pcCU->getDepth(0) != 0)
    {
      pIntegerMv2Nx2NPred = &(m_integerMv2Nx2N[eRefPicList][iRefIdxPred]);
    }
    
    // EMI: Save Block width and height in global variables, to use in our NN
    PUHeight = iRoiHeight;
    PUWidth = iRoiWidth;


    xPatternSearchFast  ( pcCU, pcPatternKey, piRefY, iRefStride, &cMvSrchRngLT, &cMvSrchRngRB, rcMv, ruiCost, pIntegerMv2Nx2NPred );
    if (pcCU->getPartitionSize(0) == SIZE_2Nx2N)
    {
      m_integerMv2Nx2N[eRefPicList][iRefIdxPred] = rcMv;
    }
  }

  m_pcRdCost->selectMotionLambda( true, 0, pcCU->getCUTransquantBypass(uiPartAddr) );

  m_pcRdCost->setCostScale ( 1 );
    
  const Bool bIsLosslessCoded = pcCU->getCUTransquantBypass(uiPartAddr) != 0;
  xPatternSearchFracDIF( bIsLosslessCoded, pcPatternKey, piRefY, iRefStride, &rcMv, cMvHalf, cMvQter, ruiCost );

  m_pcRdCost->setCostScale( 0 );

  // EMI: Big chunk of modifications!
  
  //Run our ANN model
  NN_pred();
  
  /* 
  Fractional Motion Estimation values computed by standard are stored in TComMv variables cMvHalf & cMvQter
  We create other TComMv variables, and replace the standard values with our NN predicted values
  Our NN modifies global variables MVX_HALF & MVX_QRTER, which in return are set used to set our new Mv
  */
  TComMv MV_HALF, MV_QRTER;
  MV_HALF.setHor(MVX_HALF);
  MV_HALF.setVer(MVY_HALF);
  MV_QRTER.setHor(MVX_QRTER);
  MV_QRTER.setVer(MVY_QRTER);

  // For finding Integer Motion Estimation, Set Horizontal and Vertical values to zero:

  // MV_HALF.setHor(0);
  // MV_HALF.setVer(0);
  // MV_QRTER.setHor(0);
  // MV_QRTER.setVer(0);

  /* 
  EMI: Dataset Extraction!
  To Write the errors and output MV in a CSV file:
  Real values for errors: U,V,H           - NN values for errors: IN[]
  Real values for MV: cMvHalf, cMvQter    - NN values for MV: MV_HALF, MV_QRTER
  Block Width and Height: iRoiWidth, iRoiHeight
  
  To write the values of the output class directly instead of coordinates:
  Half * 0.5 + Quarter * 0.25:  results in range from -0.75->0.75
  Add both X & Y + 0.75:        range is now 0->1.5
  Multiply X by 4:              X values are now [0, 1, 2, 3, 4, 5, 6]
  Multiply Y by 4*7=28:         Y values are now [0, 7, 14, 21, 28, 35, 42]
  Adding X+Y results in the desired output class, given that the mapping starts from 
  0 for top left corner, 24 center, and 48 for bottom right corner
  */

  // int MV_X = (((cMvHalf.getHor() * 0.5) + (cMvQter.getHor() * 0.25)) + 0.75) * 4;
  // int MV_Y = (((cMvHalf.getVer() * 0.5) + (cMvQter.getVer() * 0.25)) + 0.75) * 28;
  // int OUT_CLASS = MV_Y + MV_X;
  // ofstream errors;
  // errors.open("/home/vague/git-repos/HM16.9/DL/SSE.csv", ios::app);
  // errors << array_e[0] << ',' << array_e[1] << ',' << array_e[2] << ',' << array_e[3] << ',' << C << ',' << array_e[4] << ',' << array_e[5] << ',' << array_e[6] << ',' << array_e[7] << ',' << iRoiHeight << ',' << iRoiWidth << ',' << OUT_CLASS <<endl;

  // Replace Motion Vector with values computed by our NN

  rcMv <<= 2;
  // rcMv += (cMvHalf <<= 1); // Standard FME MV, Half
  // rcMv += cMvQter;         // Standard FME MV, Half
  rcMv += (MV_HALF <<= 1);
  rcMv += MV_QRTER;
  
  // End of modification

  UInt uiMvBits = m_pcRdCost->getBitsOfVectorWithPredictor( rcMv.getHor(), rcMv.getVer() );

  ruiBits      += uiMvBits;
  ruiCost       = (Distortion)( floor( fWeight * ( (Double)ruiCost - (Double)m_pcRdCost->getCost( uiMvBits ) ) ) + (Double)m_pcRdCost->getCost( ruiBits ) );
  
}


Void TEncSearch::xSetSearchRange ( const TComDataCU* const pcCU, const TComMv& cMvPred, const Int iSrchRng,
                                   TComMv& rcMvSrchRngLT, TComMv& rcMvSrchRngRB )
{
  Int  iMvShift = 2;
  TComMv cTmpMvPred = cMvPred;
  pcCU->clipMv( cTmpMvPred );

  rcMvSrchRngLT.setHor( cTmpMvPred.getHor() - (iSrchRng << iMvShift) );
  rcMvSrchRngLT.setVer( cTmpMvPred.getVer() - (iSrchRng << iMvShift) );

  rcMvSrchRngRB.setHor( cTmpMvPred.getHor() + (iSrchRng << iMvShift) );
  rcMvSrchRngRB.setVer( cTmpMvPred.getVer() + (iSrchRng << iMvShift) );
  pcCU->clipMv        ( rcMvSrchRngLT );
  pcCU->clipMv        ( rcMvSrchRngRB );

#if ME_ENABLE_ROUNDING_OF_MVS
  rcMvSrchRngLT.divideByPowerOf2(iMvShift);
  rcMvSrchRngRB.divideByPowerOf2(iMvShift);
#else
  rcMvSrchRngLT >>= iMvShift;
  rcMvSrchRngRB >>= iMvShift;
#endif
}


Void TEncSearch::xPatternSearch(const TComPattern* const pcPatternKey,
	const Pel*               piRefY,
	const Int                iRefStride,
	const TComMv* const      pcMvSrchRngLT,
	const TComMv* const      pcMvSrchRngRB,
	TComMv&      rcMv,
	Distortion&  ruiSAD)
{
	Int   iSrchRngHorLeft = pcMvSrchRngLT->getHor();
	Int   iSrchRngHorRight = pcMvSrchRngRB->getHor();
	Int   iSrchRngVerTop = pcMvSrchRngLT->getVer();
	Int   iSrchRngVerBottom = pcMvSrchRngRB->getVer();

	Distortion  uiSad;
	Distortion  uiSadBest = std::numeric_limits<Distortion>::max();
	Int         iBestX = 0;
	Int         iBestY = 0;


	m_pcRdCost->setDistParam(pcPatternKey, piRefY, iRefStride, m_cDistParam);

	// fast encoder decision: use subsampled SAD for integer ME
	if (m_pcEncCfg->getFastInterSearchMode() == FASTINTERSEARCH_MODE1 || m_pcEncCfg->getFastInterSearchMode() == FASTINTERSEARCH_MODE3)
	{
		if (m_cDistParam.iRows > 8)
		{
			m_cDistParam.iSubShift = 1;
		}
	}

	piRefY += (iSrchRngVerTop * iRefStride);

	for (Int y = iSrchRngVerTop; y <= iSrchRngVerBottom; y++)
	{
		for (Int x = iSrchRngHorLeft; x <= iSrchRngHorRight; x++)
		{
			//  find min. distortion position
			m_cDistParam.pCur = piRefY + x;

			setDistParamComp(COMPONENT_Y);

			m_cDistParam.bitDepth = pcPatternKey->getBitDepthY();
			uiSad = m_cDistParam.DistFunc(&m_cDistParam);

			// motion cost
			uiSad += m_pcRdCost->getCostOfVectorWithPredictor(x, y);

			if (uiSad < uiSadBest)
			{
				uiSadBest = uiSad;
				iBestX = x;
				iBestY = y;
				m_cDistParam.m_maximumDistortionForEarlyExit = uiSad;
			}
		}
		piRefY += iRefStride;
	}




		rcMv.set(iBestX, iBestY);


		ruiSAD = uiSadBest - m_pcRdCost->getCostOfVectorWithPredictor(iBestX, iBestY);

		//getchar();
		return;
	}


Void TEncSearch::xPatternSearchFast( const TComDataCU* const  pcCU,
                                     const TComPattern* const pcPatternKey,
                                     const Pel* const         piRefY,
                                     const Int                iRefStride,
                                     const TComMv* const      pcMvSrchRngLT,
                                     const TComMv* const      pcMvSrchRngRB,
                                     TComMv&                  rcMv,
                                     Distortion&              ruiSAD,
                                     const TComMv* const      pIntegerMv2Nx2NPred )
{
  assert (MD_LEFT < NUM_MV_PREDICTORS);
  pcCU->getMvPredLeft       ( m_acMvPredictors[MD_LEFT] );
  assert (MD_ABOVE < NUM_MV_PREDICTORS);
  pcCU->getMvPredAbove      ( m_acMvPredictors[MD_ABOVE] );
  assert (MD_ABOVE_RIGHT < NUM_MV_PREDICTORS);
  pcCU->getMvPredAboveRight ( m_acMvPredictors[MD_ABOVE_RIGHT] );

  switch ( m_motionEstimationSearchMethod )
  {
    case MESEARCH_DIAMOND:
      xTZSearch( pcCU, pcPatternKey, piRefY, iRefStride, pcMvSrchRngLT, pcMvSrchRngRB, rcMv, ruiSAD, pIntegerMv2Nx2NPred, false );
	     
      break;

    case MESEARCH_SELECTIVE:
      xTZSearchSelective( pcCU, pcPatternKey, piRefY, iRefStride, pcMvSrchRngLT, pcMvSrchRngRB, rcMv, ruiSAD, pIntegerMv2Nx2NPred );
      break;

    case MESEARCH_DIAMOND_ENHANCED:
      xTZSearch( pcCU, pcPatternKey, piRefY, iRefStride, pcMvSrchRngLT, pcMvSrchRngRB, rcMv, ruiSAD, pIntegerMv2Nx2NPred, true );
      break;

    case MESEARCH_FULL: // shouldn't get here.
    default:
      break;
  }
}


Void TEncSearch::xTZSearch( const TComDataCU* const pcCU,
                            const TComPattern* const pcPatternKey,
                            const Pel* const         piRefY,
                            const Int                iRefStride,
                            const TComMv* const      pcMvSrchRngLT,
                            const TComMv* const      pcMvSrchRngRB,
                            TComMv&                  rcMv,
                            Distortion&              ruiSAD,
                            const TComMv* const      pIntegerMv2Nx2NPred,
                            const Bool               bExtendedSettings)
{
  const Bool bUseAdaptiveRaster                      = bExtendedSettings;
  const Int  iRaster                                 = 5;
  const Bool bTestOtherPredictedMV                   = bExtendedSettings;
  const Bool bTestZeroVector                         = true;
  const Bool bTestZeroVectorStart                    = bExtendedSettings;
  const Bool bTestZeroVectorStop                     = false;
  const Bool bFirstSearchDiamond                     = true;  // 1 = xTZ8PointDiamondSearch   0 = xTZ8PointSquareSearch
  const Bool bFirstCornersForDiamondDist1            = bExtendedSettings;
  const Bool bFirstSearchStop                        = m_pcEncCfg->getFastMEAssumingSmootherMVEnabled();
  const UInt uiFirstSearchRounds                     = 3;     // first search stop X rounds after best match (must be >=1)
  const Bool bEnableRasterSearch                     = true;
  const Bool bAlwaysRasterSearch                     = bExtendedSettings;  // true: BETTER but factor 2 slower
  const Bool bRasterRefinementEnable                 = false; // enable either raster refinement or star refinement
  const Bool bRasterRefinementDiamond                = false; // 1 = xTZ8PointDiamondSearch   0 = xTZ8PointSquareSearch
  const Bool bRasterRefinementCornersForDiamondDist1 = bExtendedSettings;
  const Bool bStarRefinementEnable                   = true;  // enable either star refinement or raster refinement
  const Bool bStarRefinementDiamond                  = true;  // 1 = xTZ8PointDiamondSearch   0 = xTZ8PointSquareSearch
  const Bool bStarRefinementCornersForDiamondDist1   = bExtendedSettings;
  const Bool bStarRefinementStop                     = false;
  const UInt uiStarRefinementRounds                  = 2;  // star refinement stop X rounds after best match (must be >=1)
  const Bool bNewZeroNeighbourhoodTest               = bExtendedSettings;

  UInt uiSearchRange = m_iSearchRange;
  pcCU->clipMv( rcMv );
#if ME_ENABLE_ROUNDING_OF_MVS
  rcMv.divideByPowerOf2(2);
#else
  rcMv >>= 2;
#endif
  // init TZSearchStruct
  IntTZSearchStruct cStruct;
  cStruct.iYStride    = iRefStride;
  cStruct.piRefY      = piRefY;
  cStruct.uiBestSad   = MAX_UINT;

  // set rcMv (Median predictor) as start point and as best point
  xTZSearchHelp( pcPatternKey, cStruct, rcMv.getHor(), rcMv.getVer(), 0, 0 );

  // test whether one of PRED_A, PRED_B, PRED_C MV is better start point than Median predictor
  if ( bTestOtherPredictedMV )
  {
    for ( UInt index = 0; index < NUM_MV_PREDICTORS; index++ )
    {
      TComMv cMv = m_acMvPredictors[index];
      pcCU->clipMv( cMv );
#if ME_ENABLE_ROUNDING_OF_MVS
      cMv.divideByPowerOf2(2);
#else
      cMv >>= 2;
#endif
      if (cMv != rcMv && (cMv.getHor() != cStruct.iBestX && cMv.getVer() != cStruct.iBestY))
      {
        // only test cMV if not obviously previously tested.
        xTZSearchHelp( pcPatternKey, cStruct, cMv.getHor(), cMv.getVer(), 0, 0 );
      }
    }
  }

  // test whether zero Mv is better start point than Median predictor
  if ( bTestZeroVector )
  {
    if ((rcMv.getHor() != 0 || rcMv.getVer() != 0) &&
        (0 != cStruct.iBestX || 0 != cStruct.iBestY))
    {
      // only test 0-vector if not obviously previously tested.
      xTZSearchHelp( pcPatternKey, cStruct, 0, 0, 0, 0 );
    }
  }

  Int   iSrchRngHorLeft   = pcMvSrchRngLT->getHor();
  Int   iSrchRngHorRight  = pcMvSrchRngRB->getHor();
  Int   iSrchRngVerTop    = pcMvSrchRngLT->getVer();
  Int   iSrchRngVerBottom = pcMvSrchRngRB->getVer();

  if (pIntegerMv2Nx2NPred != 0)
  {
    TComMv integerMv2Nx2NPred = *pIntegerMv2Nx2NPred;
    integerMv2Nx2NPred <<= 2;
    pcCU->clipMv( integerMv2Nx2NPred );
#if ME_ENABLE_ROUNDING_OF_MVS
    integerMv2Nx2NPred.divideByPowerOf2(2);
#else
    integerMv2Nx2NPred >>= 2;
#endif
    if ((rcMv != integerMv2Nx2NPred) &&
        (integerMv2Nx2NPred.getHor() != cStruct.iBestX || integerMv2Nx2NPred.getVer() != cStruct.iBestY))
    {
      // only test integerMv2Nx2NPred if not obviously previously tested.
      xTZSearchHelp(pcPatternKey, cStruct, integerMv2Nx2NPred.getHor(), integerMv2Nx2NPred.getVer(), 0, 0);
    }

    // reset search range
    TComMv cMvSrchRngLT;
    TComMv cMvSrchRngRB;
    Int iSrchRng = m_iSearchRange;
    TComMv currBestMv(cStruct.iBestX, cStruct.iBestY );
    currBestMv <<= 2;
    xSetSearchRange( pcCU, currBestMv, iSrchRng, cMvSrchRngLT, cMvSrchRngRB );
    iSrchRngHorLeft   = cMvSrchRngLT.getHor();
    iSrchRngHorRight  = cMvSrchRngRB.getHor();
    iSrchRngVerTop    = cMvSrchRngLT.getVer();
    iSrchRngVerBottom = cMvSrchRngRB.getVer();
  }

  // start search
  Int  iDist = 0;
  Int  iStartX = cStruct.iBestX;
  Int  iStartY = cStruct.iBestY;

  const Bool bBestCandidateZero = (cStruct.iBestX == 0) && (cStruct.iBestY == 0);

  // first search around best position up to now.
  // The following works as a "subsampled/log" window search around the best candidate
  for (iDist = 1; iDist <= (Int)uiSearchRange; iDist *= 2)
	  
  {
    if ( bFirstSearchDiamond == 1 )
    {
      xTZ8PointDiamondSearch ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist, bFirstCornersForDiamondDist1 );
    }
    else
    {
      xTZ8PointSquareSearch  ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist );
    }

    if ( bFirstSearchStop && ( cStruct.uiBestRound >= uiFirstSearchRounds ) ) // stop criterion
    {
      break;
    }
  }

  if (!bNewZeroNeighbourhoodTest)
  {
    // test whether zero Mv is a better start point than Median predictor
    if ( bTestZeroVectorStart && ((cStruct.iBestX != 0) || (cStruct.iBestY != 0)) )
    {
      xTZSearchHelp( pcPatternKey, cStruct, 0, 0, 0, 0 );
      if ( (cStruct.iBestX == 0) && (cStruct.iBestY == 0) )
      {
        // test its neighborhood
        for ( iDist = 1; iDist <= (Int)uiSearchRange; iDist*=2 )
        {
          xTZ8PointDiamondSearch( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, 0, 0, iDist, false );
          if ( bTestZeroVectorStop && (cStruct.uiBestRound > 0) ) // stop criterion
          {
            break;
          }
        }
      }
    }
  }
  else
  {
    // Test also zero neighbourhood but with half the range
    // It was reported that the original (above) search scheme using bTestZeroVectorStart did not
    // make sense since one would have already checked the zero candidate earlier
    // and thus the conditions for that test would have not been satisfied
    if (bTestZeroVectorStart == true && bBestCandidateZero != true)
    {
      for ( iDist = 1; iDist <= ((Int)uiSearchRange >> 1); iDist*=2 )
      {
        xTZ8PointDiamondSearch( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, 0, 0, iDist, false );
        if ( bTestZeroVectorStop && (cStruct.uiBestRound > 2) ) // stop criterion
        {
          break;
        }
      }
    }
  }

  // calculate only 2 missing points instead 8 points if cStruct.uiBestDistance == 1
  if ( cStruct.uiBestDistance == 1 )
  {
    cStruct.uiBestDistance = 0;
    xTZ2PointSearch( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB );
  }

  // raster search if distance is too big
  if (bUseAdaptiveRaster)
  {
    int iWindowSize = iRaster;
    Int   iSrchRngRasterLeft   = iSrchRngHorLeft;
    Int   iSrchRngRasterRight  = iSrchRngHorRight;
    Int   iSrchRngRasterTop    = iSrchRngVerTop;
    Int   iSrchRngRasterBottom = iSrchRngVerBottom;

    if (!(bEnableRasterSearch && ( ((Int)(cStruct.uiBestDistance) > iRaster))))
    {
      iWindowSize ++;
      iSrchRngRasterLeft /= 2;
      iSrchRngRasterRight /= 2;
      iSrchRngRasterTop /= 2;
      iSrchRngRasterBottom /= 2;
    }
    cStruct.uiBestDistance = iWindowSize;
    for ( iStartY = iSrchRngRasterTop; iStartY <= iSrchRngRasterBottom; iStartY += iWindowSize )
    {
      for ( iStartX = iSrchRngRasterLeft; iStartX <= iSrchRngRasterRight; iStartX += iWindowSize )
      {
        xTZSearchHelp( pcPatternKey, cStruct, iStartX, iStartY, 0, iWindowSize );
      }
    }
  }
  else
  {
    if ( bEnableRasterSearch && ( ((Int)(cStruct.uiBestDistance) > iRaster) || bAlwaysRasterSearch ) )
    {
      cStruct.uiBestDistance = iRaster;
      for ( iStartY = iSrchRngVerTop; iStartY <= iSrchRngVerBottom; iStartY += iRaster )
      {
        for ( iStartX = iSrchRngHorLeft; iStartX <= iSrchRngHorRight; iStartX += iRaster )
        {
          xTZSearchHelp( pcPatternKey, cStruct, iStartX, iStartY, 0, iRaster );
        }
      }
    }
  }

  // raster refinement

  if ( bRasterRefinementEnable && cStruct.uiBestDistance > 0 )
  {
    while ( cStruct.uiBestDistance > 0 )
    {
      iStartX = cStruct.iBestX;
      iStartY = cStruct.iBestY;
      if ( cStruct.uiBestDistance > 1 )
      {
        iDist = cStruct.uiBestDistance >>= 1;
        if ( bRasterRefinementDiamond == 1 )
        {
          xTZ8PointDiamondSearch ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist, bRasterRefinementCornersForDiamondDist1 );
        }
        else
        {
          xTZ8PointSquareSearch  ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist );
        }
      }

      // calculate only 2 missing points instead 8 points if cStruct.uiBestDistance == 1
      if ( cStruct.uiBestDistance == 1 )
      {
        cStruct.uiBestDistance = 0;
        if ( cStruct.ucPointNr != 0 )
        {
          xTZ2PointSearch( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB );
        }
      }
    }
  }

  // star refinement
  if ( bStarRefinementEnable && cStruct.uiBestDistance > 0 )
  {
    while ( cStruct.uiBestDistance > 0 )
    {
      iStartX = cStruct.iBestX;
      iStartY = cStruct.iBestY;
      cStruct.uiBestDistance = 0;
      cStruct.ucPointNr = 0;
      for ( iDist = 1; iDist < (Int)uiSearchRange + 1; iDist*=2 )
      {
        if ( bStarRefinementDiamond == 1 )
        {
          xTZ8PointDiamondSearch ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist, bStarRefinementCornersForDiamondDist1 );
        }
        else
        {
          xTZ8PointSquareSearch  ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist );
        }
        if ( bStarRefinementStop && (cStruct.uiBestRound >= uiStarRefinementRounds) ) // stop criterion
        {
          break;
        }
      }

      // calculate only 2 missing points instead 8 points if cStrukt.uiBestDistance == 1
      if ( cStruct.uiBestDistance == 1 )
      {
        cStruct.uiBestDistance = 0;
        if ( cStruct.ucPointNr != 0 )
        {
          xTZ2PointSearch( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB );
        }
      }
    }
  }


  // EMI: Storing the 8 integer SSE points

  iDist = 1;
  iStartX = cStruct.iBestX;
  iStartY = cStruct.iBestY;
  // Notice that we set the save flag to true
  xTZ8PointSquareSearch(pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist, true);
  
  // END OF MODIFICATION

  // write out best match
  rcMv.set( cStruct.iBestX, cStruct.iBestY );
  ruiSAD = cStruct.uiBestSad - m_pcRdCost->getCostOfVectorWithPredictor( cStruct.iBestX, cStruct.iBestY );
  C = ruiSAD;  // EMI: Storing the value of the best integer location "center"
}


Void TEncSearch::xTZSearchSelective( const TComDataCU* const   pcCU,
                                     const TComPattern* const  pcPatternKey,
                                     const Pel* const          piRefY,
                                     const Int                 iRefStride,
                                     const TComMv* const       pcMvSrchRngLT,
                                     const TComMv* const       pcMvSrchRngRB,
                                     TComMv                   &rcMv,
                                     Distortion               &ruiSAD,
                                     const TComMv* const       pIntegerMv2Nx2NPred )
{
  const Bool bTestOtherPredictedMV    = true;
  const Bool bTestZeroVector          = true;
  const Bool bEnableRasterSearch      = true;
  const Bool bAlwaysRasterSearch      = false;  // 1: BETTER but factor 15x slower
  const Bool bStarRefinementEnable    = true;   // enable either star refinement or raster refinement
  const Bool bStarRefinementDiamond   = true;   // 1 = xTZ8PointDiamondSearch   0 = xTZ8PointSquareSearch
  const Bool bStarRefinementStop      = false;
  const UInt uiStarRefinementRounds   = 2;  // star refinement stop X rounds after best match (must be >=1)
  const UInt uiSearchRange            = m_iSearchRange;
  const Int  uiSearchRangeInitial     = m_iSearchRange >> 2;
  const Int  uiSearchStep             = 4;
  const Int  iMVDistThresh            = 8;

  Int   iSrchRngHorLeft         = pcMvSrchRngLT->getHor();
  Int   iSrchRngHorRight        = pcMvSrchRngRB->getHor();
  Int   iSrchRngVerTop          = pcMvSrchRngLT->getVer();
  Int   iSrchRngVerBottom       = pcMvSrchRngRB->getVer();
  Int   iFirstSrchRngHorLeft    = 0;
  Int   iFirstSrchRngHorRight   = 0;
  Int   iFirstSrchRngVerTop     = 0;
  Int   iFirstSrchRngVerBottom  = 0;
  Int   iStartX                 = 0;
  Int   iStartY                 = 0;
  Int   iBestX                  = 0;
  Int   iBestY                  = 0;
  Int   iDist                   = 0;

  pcCU->clipMv( rcMv );
#if ME_ENABLE_ROUNDING_OF_MVS
  rcMv.divideByPowerOf2(2);
#else
  rcMv >>= 2;
#endif
  // init TZSearchStruct
  IntTZSearchStruct cStruct;
  cStruct.iYStride    = iRefStride;
  cStruct.piRefY      = piRefY;
  cStruct.uiBestSad   = MAX_UINT;
  cStruct.iBestX = 0;
  cStruct.iBestY = 0;


  // set rcMv (Median predictor) as start point and as best point
  xTZSearchHelp( pcPatternKey, cStruct, rcMv.getHor(), rcMv.getVer(), 0, 0 );

  // test whether one of PRED_A, PRED_B, PRED_C MV is better start point than Median predictor
  if ( bTestOtherPredictedMV )
  {
    for ( UInt index = 0; index < NUM_MV_PREDICTORS; index++ )
    {
      TComMv cMv = m_acMvPredictors[index];
      pcCU->clipMv( cMv );
#if ME_ENABLE_ROUNDING_OF_MVS
      cMv.divideByPowerOf2(2);
#else
      cMv >>= 2;
#endif
      xTZSearchHelp( pcPatternKey, cStruct, cMv.getHor(), cMv.getVer(), 0, 0 );
    }
  }

  // test whether zero Mv is better start point than Median predictor
  if ( bTestZeroVector )
  {
    xTZSearchHelp( pcPatternKey, cStruct, 0, 0, 0, 0 );
  }

  if ( pIntegerMv2Nx2NPred != 0 )
  {
    TComMv integerMv2Nx2NPred = *pIntegerMv2Nx2NPred;
    integerMv2Nx2NPred <<= 2;
    pcCU->clipMv( integerMv2Nx2NPred );
#if ME_ENABLE_ROUNDING_OF_MVS
    integerMv2Nx2NPred.divideByPowerOf2(2);
#else
    integerMv2Nx2NPred >>= 2;
#endif
    xTZSearchHelp(pcPatternKey, cStruct, integerMv2Nx2NPred.getHor(), integerMv2Nx2NPred.getVer(), 0, 0);

    // reset search range
    TComMv cMvSrchRngLT;
    TComMv cMvSrchRngRB;
    Int iSrchRng = m_iSearchRange;
    TComMv currBestMv(cStruct.iBestX, cStruct.iBestY );
    currBestMv <<= 2;
    xSetSearchRange( pcCU, currBestMv, iSrchRng, cMvSrchRngLT, cMvSrchRngRB );
    iSrchRngHorLeft   = cMvSrchRngLT.getHor();
    iSrchRngHorRight  = cMvSrchRngRB.getHor();
    iSrchRngVerTop    = cMvSrchRngLT.getVer();
    iSrchRngVerBottom = cMvSrchRngRB.getVer();
  }

  // Initial search
  iBestX = cStruct.iBestX;
  iBestY = cStruct.iBestY; 
  iFirstSrchRngHorLeft    = ((iBestX - uiSearchRangeInitial) > iSrchRngHorLeft)   ? (iBestX - uiSearchRangeInitial) : iSrchRngHorLeft;
  iFirstSrchRngVerTop     = ((iBestY - uiSearchRangeInitial) > iSrchRngVerTop)    ? (iBestY - uiSearchRangeInitial) : iSrchRngVerTop;
  iFirstSrchRngHorRight   = ((iBestX + uiSearchRangeInitial) < iSrchRngHorRight)  ? (iBestX + uiSearchRangeInitial) : iSrchRngHorRight;  
  iFirstSrchRngVerBottom  = ((iBestY + uiSearchRangeInitial) < iSrchRngVerBottom) ? (iBestY + uiSearchRangeInitial) : iSrchRngVerBottom;    

  for ( iStartY = iFirstSrchRngVerTop; iStartY <= iFirstSrchRngVerBottom; iStartY += uiSearchStep )
  {
    for ( iStartX = iFirstSrchRngHorLeft; iStartX <= iFirstSrchRngHorRight; iStartX += uiSearchStep )
    {
      xTZSearchHelp( pcPatternKey, cStruct, iStartX, iStartY, 0, 0 );
      xTZ8PointDiamondSearch ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, 1, false );
      xTZ8PointDiamondSearch ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, 2, false );
    }
  }

  Int iMaxMVDistToPred = (abs(cStruct.iBestX - iBestX) > iMVDistThresh || abs(cStruct.iBestY - iBestY) > iMVDistThresh);

  //full search with early exit if MV is distant from predictors
  if ( bEnableRasterSearch && (iMaxMVDistToPred || bAlwaysRasterSearch) )
  {
    for ( iStartY = iSrchRngVerTop; iStartY <= iSrchRngVerBottom; iStartY += 1 )
    {
      for ( iStartX = iSrchRngHorLeft; iStartX <= iSrchRngHorRight; iStartX += 1 )
      {
        xTZSearchHelp( pcPatternKey, cStruct, iStartX, iStartY, 0, 1 );
      }
    }
  }
  //Smaller MV, refine around predictor
  else if ( bStarRefinementEnable && cStruct.uiBestDistance > 0 )
  {
    // start refinement
    while ( cStruct.uiBestDistance > 0 )
    {
      iStartX = cStruct.iBestX;
      iStartY = cStruct.iBestY;
      cStruct.uiBestDistance = 0;
      cStruct.ucPointNr = 0;
      for ( iDist = 1; iDist < (Int)uiSearchRange + 1; iDist*=2 )
      {
        if ( bStarRefinementDiamond == 1 )
        {
          xTZ8PointDiamondSearch ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist, false );
        }
        else
        {
          xTZ8PointSquareSearch  ( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB, iStartX, iStartY, iDist );
        }
        if ( bStarRefinementStop && (cStruct.uiBestRound >= uiStarRefinementRounds) ) // stop criterion
        {
          break;
        }
      }

      // calculate only 2 missing points instead 8 points if cStrukt.uiBestDistance == 1
      if ( cStruct.uiBestDistance == 1 )
      {
        cStruct.uiBestDistance = 0;
        if ( cStruct.ucPointNr != 0 )
        {
          xTZ2PointSearch( pcPatternKey, cStruct, pcMvSrchRngLT, pcMvSrchRngRB );
        }
      }
    }
  }

  // write out best match
  rcMv.set( cStruct.iBestX, cStruct.iBestY );
  ruiSAD = cStruct.uiBestSad - m_pcRdCost->getCostOfVectorWithPredictor( cStruct.iBestX, cStruct.iBestY );

}


Void TEncSearch::xPatternSearchFracDIF(
                                       Bool         bIsLosslessCoded,
                                       TComPattern* pcPatternKey,
                                       Pel*         piRefY,
                                       Int          iRefStride,
                                       TComMv*      pcMvInt,
                                       TComMv&      rcMvHalf,
                                       TComMv&      rcMvQter,
                                       Distortion&  ruiCost
                                      )
{
  //  Reference pattern initialization (integer scale)
	
  TComPattern cPatternRoi;
  Int         iOffset    = pcMvInt->getHor() + pcMvInt->getVer() * iRefStride;
  cPatternRoi.initPattern(piRefY + iOffset,
                          pcPatternKey->getROIYWidth(),
                          pcPatternKey->getROIYHeight(),
                          iRefStride,
                          pcPatternKey->getBitDepthY());

  //  Half-pel refinement
  xExtDIFUpSamplingH ( &cPatternRoi );

  rcMvHalf = *pcMvInt;   rcMvHalf <<= 1;    // for mv-cost
  TComMv baseRefMv(0, 0);
  ruiCost = xPatternRefinement( pcPatternKey, baseRefMv, 2, rcMvHalf, !bIsLosslessCoded );

  m_pcRdCost->setCostScale( 0 );

  xExtDIFUpSamplingQ ( &cPatternRoi, rcMvHalf );
  baseRefMv = rcMvHalf;
  baseRefMv <<= 1;

  rcMvQter = *pcMvInt;   rcMvQter <<= 1;    // for mv-cost
  rcMvQter += rcMvHalf;  rcMvQter <<= 1;
  ruiCost = xPatternRefinement( pcPatternKey, baseRefMv, 1, rcMvQter, !bIsLosslessCoded );
}


//! encode residual and calculate rate-distortion for a CU block
Void TEncSearch::encodeResAndCalcRdInterCU( TComDataCU* pcCU, TComYuv* pcYuvOrg, TComYuv* pcYuvPred,
                                            TComYuv* pcYuvResi, TComYuv* pcYuvResiBest, TComYuv* pcYuvRec,
                                            Bool bSkipResidual DEBUG_STRING_FN_DECLARE(sDebug) )
{
  assert ( !pcCU->isIntra(0) );

  const UInt cuWidthPixels      = pcCU->getWidth ( 0 );
  const UInt cuHeightPixels     = pcCU->getHeight( 0 );
  const Int  numValidComponents = pcCU->getPic()->getNumberValidComponents();
  const TComSPS &sps=*(pcCU->getSlice()->getSPS());

  // The pcCU is not marked as skip-mode at this point, and its m_pcTrCoeff, m_pcArlCoeff, m_puhCbf, m_puhTrIdx will all be 0.
  // due to prior calls to TComDataCU::initEstData(  );

  if ( bSkipResidual ) //  No residual coding : SKIP mode
  {
    pcCU->setSkipFlagSubParts( true, 0, pcCU->getDepth(0) );

    pcYuvResi->clear();

    pcYuvPred->copyToPartYuv( pcYuvRec, 0 );
    Distortion distortion = 0;

    for (Int comp=0; comp < numValidComponents; comp++)
    {
      const ComponentID compID=ComponentID(comp);
      const UInt csx=pcYuvOrg->getComponentScaleX(compID);
      const UInt csy=pcYuvOrg->getComponentScaleY(compID);
      distortion += m_pcRdCost->getDistPart( sps.getBitDepth(toChannelType(compID)), pcYuvRec->getAddr(compID), pcYuvRec->getStride(compID), pcYuvOrg->getAddr(compID),
                                               pcYuvOrg->getStride(compID), cuWidthPixels >> csx, cuHeightPixels >> csy, compID);
    }

    m_pcRDGoOnSbacCoder->load(m_pppcRDSbacCoder[pcCU->getDepth(0)][CI_CURR_BEST]);
    m_pcEntropyCoder->resetBits();

    if (pcCU->getSlice()->getPPS()->getTransquantBypassEnableFlag())
    {
      m_pcEntropyCoder->encodeCUTransquantBypassFlag(pcCU, 0, true);
    }

    m_pcEntropyCoder->encodeSkipFlag(pcCU, 0, true);
    m_pcEntropyCoder->encodeMergeIndex( pcCU, 0, true );

    UInt uiBits = m_pcEntropyCoder->getNumberOfWrittenBits();
    pcCU->getTotalBits()       = uiBits;
    pcCU->getTotalDistortion() = distortion;
    pcCU->getTotalCost()       = m_pcRdCost->calcRdCost( uiBits, distortion );

    m_pcRDGoOnSbacCoder->store(m_pppcRDSbacCoder[pcCU->getDepth(0)][CI_TEMP_BEST]);

#if DEBUG_STRING
    pcYuvResiBest->clear(); // Clear the residual image, if we didn't code it.
    for(UInt i=0; i<MAX_NUM_COMPONENT+1; i++)
    {
      sDebug+=debug_reorder_data_inter_token[i];
    }
#endif

    return;
  }

  //  Residual coding.

   pcYuvResi->subtract( pcYuvOrg, pcYuvPred, 0, cuWidthPixels );

  TComTURecurse tuLevel0(pcCU, 0);

  Double     nonZeroCost       = 0;
  UInt       nonZeroBits       = 0;
  Distortion nonZeroDistortion = 0;
  Distortion zeroDistortion    = 0;

  m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[ pcCU->getDepth( 0 ) ][ CI_CURR_BEST ] );

  xEstimateInterResidualQT( pcYuvResi,  nonZeroCost, nonZeroBits, nonZeroDistortion, &zeroDistortion, tuLevel0 DEBUG_STRING_PASS_INTO(sDebug) );

  // -------------------------------------------------------
  // set the coefficients in the pcCU, and also calculates the residual data.
  // If a block full of 0's is efficient, then just use 0's.
  // The costs at this point do not include header bits.

  m_pcEntropyCoder->resetBits();
  m_pcEntropyCoder->encodeQtRootCbfZero( );
  const UInt   zeroResiBits = m_pcEntropyCoder->getNumberOfWrittenBits();
  const Double zeroCost     = (pcCU->isLosslessCoded( 0 )) ? (nonZeroCost+1) : (m_pcRdCost->calcRdCost( zeroResiBits, zeroDistortion ));

  if ( zeroCost < nonZeroCost || !pcCU->getQtRootCbf(0) )
  {
    const UInt uiQPartNum = tuLevel0.GetAbsPartIdxNumParts();
    ::memset( pcCU->getTransformIdx()     , 0, uiQPartNum * sizeof(UChar) );
    for (Int comp=0; comp < numValidComponents; comp++)
    {
      const ComponentID component = ComponentID(comp);
      ::memset( pcCU->getCbf( component ) , 0, uiQPartNum * sizeof(UChar) );
      ::memset( pcCU->getCrossComponentPredictionAlpha(component), 0, ( uiQPartNum * sizeof(SChar) ) );
    }
    static const UInt useTS[MAX_NUM_COMPONENT]={0,0,0};
    pcCU->setTransformSkipSubParts ( useTS, 0, pcCU->getDepth(0) );
#if DEBUG_STRING
    sDebug.clear();
    for(UInt i=0; i<MAX_NUM_COMPONENT+1; i++)
    {
      sDebug+=debug_reorder_data_inter_token[i];
    }
#endif
  }
  else
  {
    xSetInterResidualQTData( NULL, false, tuLevel0); // Call first time to set coefficients.
  }

  // all decisions now made. Fully encode the CU, including the headers:
  m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[pcCU->getDepth(0)][CI_CURR_BEST] );

  UInt finalBits = 0;
  xAddSymbolBitsInter( pcCU, finalBits );
  // we've now encoded the pcCU, and so have a valid bit cost

  if ( !pcCU->getQtRootCbf( 0 ) )
  {
    pcYuvResiBest->clear(); // Clear the residual image, if we didn't code it.
  }
  else
  {
    xSetInterResidualQTData( pcYuvResiBest, true, tuLevel0 ); // else set the residual image data pcYUVResiBest from the various temp images.
  }
  m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ pcCU->getDepth( 0 ) ][ CI_TEMP_BEST ] );

  pcYuvRec->addClip ( pcYuvPred, pcYuvResiBest, 0, cuWidthPixels, sps.getBitDepths() );

  // update with clipped distortion and cost (previously unclipped reconstruction values were used)

  Distortion finalDistortion = 0;
  for(Int comp=0; comp<numValidComponents; comp++)
  {
    const ComponentID compID=ComponentID(comp);
    finalDistortion += m_pcRdCost->getDistPart( sps.getBitDepth(toChannelType(compID)), pcYuvRec->getAddr(compID ), pcYuvRec->getStride(compID ), pcYuvOrg->getAddr(compID ), pcYuvOrg->getStride(compID), cuWidthPixels >> pcYuvOrg->getComponentScaleX(compID), cuHeightPixels >> pcYuvOrg->getComponentScaleY(compID), compID);
  }

  pcCU->getTotalBits()       = finalBits;
  pcCU->getTotalDistortion() = finalDistortion;
  pcCU->getTotalCost()       = m_pcRdCost->calcRdCost( finalBits, finalDistortion );
}



Void TEncSearch::xEstimateInterResidualQT( TComYuv    *pcResi,
                                           Double     &rdCost,
                                           UInt       &ruiBits,
                                           Distortion &ruiDist,
                                           Distortion *puiZeroDist,
                                           TComTU     &rTu
                                           DEBUG_STRING_FN_DECLARE(sDebug) )
{
  TComDataCU *pcCU        = rTu.getCU();
  const UInt uiAbsPartIdx = rTu.GetAbsPartIdxTU();
  const UInt uiDepth      = rTu.GetTransformDepthTotal();
  const UInt uiTrMode     = rTu.GetTransformDepthRel();
  const UInt subTUDepth   = uiTrMode + 1;
  const UInt numValidComp = pcCU->getPic()->getNumberValidComponents();
  DEBUG_STRING_NEW(sSingleStringComp[MAX_NUM_COMPONENT])

  assert( pcCU->getDepth( 0 ) == pcCU->getDepth( uiAbsPartIdx ) );
  const UInt uiLog2TrSize = rTu.GetLog2LumaTrSize();

  UInt SplitFlag = ((pcCU->getSlice()->getSPS()->getQuadtreeTUMaxDepthInter() == 1) && pcCU->isInter(uiAbsPartIdx) && ( pcCU->getPartitionSize(uiAbsPartIdx) != SIZE_2Nx2N ));
#if DEBUG_STRING
  const Int debugPredModeMask = DebugStringGetPredModeMask(pcCU->getPredictionMode(uiAbsPartIdx));
#endif

  Bool bCheckFull;

  if ( SplitFlag && uiDepth == pcCU->getDepth(uiAbsPartIdx) && ( uiLog2TrSize >  pcCU->getQuadtreeTULog2MinSizeInCU(uiAbsPartIdx) ) )
  {
    bCheckFull = false;
  }
  else
  {
    bCheckFull =  ( uiLog2TrSize <= pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() );
  }

  const Bool bCheckSplit  = ( uiLog2TrSize >  pcCU->getQuadtreeTULog2MinSizeInCU(uiAbsPartIdx) );

  assert( bCheckFull || bCheckSplit );

  // code full block
  Double     dSingleCost = MAX_DOUBLE;
  UInt       uiSingleBits                                                                                                        = 0;
  Distortion uiSingleDistComp            [MAX_NUM_COMPONENT][2/*0 = top (or whole TU for non-4:2:2) sub-TU, 1 = bottom sub-TU*/] = {{0,0},{0,0},{0,0}};
  Distortion uiSingleDist                                                                                                        = 0;
  TCoeff     uiAbsSum                    [MAX_NUM_COMPONENT][2/*0 = top (or whole TU for non-4:2:2) sub-TU, 1 = bottom sub-TU*/] = {{0,0},{0,0},{0,0}};
  UInt       uiBestTransformMode         [MAX_NUM_COMPONENT][2/*0 = top (or whole TU for non-4:2:2) sub-TU, 1 = bottom sub-TU*/] = {{0,0},{0,0},{0,0}};
  //  Stores the best explicit RDPCM mode for a TU encoded without split
  UInt       bestExplicitRdpcmModeUnSplit[MAX_NUM_COMPONENT][2/*0 = top (or whole TU for non-4:2:2) sub-TU, 1 = bottom sub-TU*/] = {{3,3}, {3,3}, {3,3}};
  SChar      bestCrossCPredictionAlpha   [MAX_NUM_COMPONENT][2/*0 = top (or whole TU for non-4:2:2) sub-TU, 1 = bottom sub-TU*/] = {{0,0},{0,0},{0,0}};

  m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ uiDepth ][ CI_QT_TRAFO_ROOT ] );

  if( bCheckFull )
  {
    Double minCost[MAX_NUM_COMPONENT][2/*0 = top (or whole TU for non-4:2:2) sub-TU, 1 = bottom sub-TU*/];
    Bool checkTransformSkip[MAX_NUM_COMPONENT];
    pcCU->setTrIdxSubParts( uiTrMode, uiAbsPartIdx, uiDepth );

    m_pcEntropyCoder->resetBits();

    memset( m_pTempPel, 0, sizeof( Pel ) * rTu.getRect(COMPONENT_Y).width * rTu.getRect(COMPONENT_Y).height ); // not necessary needed for inside of recursion (only at the beginning)

    const UInt uiQTTempAccessLayer = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() - uiLog2TrSize;
    TCoeff *pcCoeffCurr[MAX_NUM_COMPONENT];
#if ADAPTIVE_QP_SELECTION
    TCoeff *pcArlCoeffCurr[MAX_NUM_COMPONENT];
#endif

    for(UInt i=0; i<numValidComp; i++)
    {
      minCost[i][0] = MAX_DOUBLE;
      minCost[i][1] = MAX_DOUBLE;
    }

    Pel crossCPredictedResidualBuffer[ MAX_TU_SIZE * MAX_TU_SIZE ];

    for(UInt i=0; i<numValidComp; i++)
    {
      checkTransformSkip[i]=false;
      const ComponentID compID=ComponentID(i);
      const Int channelBitDepth=pcCU->getSlice()->getSPS()->getBitDepth(toChannelType(compID));
      pcCoeffCurr[compID]    = m_ppcQTTempCoeff[compID][uiQTTempAccessLayer] + rTu.getCoefficientOffset(compID);
#if ADAPTIVE_QP_SELECTION
      pcArlCoeffCurr[compID] = m_ppcQTTempArlCoeff[compID ][uiQTTempAccessLayer] +  rTu.getCoefficientOffset(compID);
#endif

      if(rTu.ProcessComponentSection(compID))
      {
        const QpParam cQP(*pcCU, compID);

        checkTransformSkip[compID] = pcCU->getSlice()->getPPS()->getUseTransformSkip() &&
                                     TUCompRectHasAssociatedTransformSkipFlag(rTu.getRect(compID), pcCU->getSlice()->getPPS()->getPpsRangeExtension().getLog2MaxTransformSkipBlockSize()) &&
                                     (!pcCU->isLosslessCoded(0));

        const Bool splitIntoSubTUs = rTu.getRect(compID).width != rTu.getRect(compID).height;

        TComTURecurse TUIterator(rTu, false, (splitIntoSubTUs ? TComTU::VERTICAL_SPLIT : TComTU::DONT_SPLIT), true, compID);

        const UInt partIdxesPerSubTU = TUIterator.GetAbsPartIdxNumParts(compID);

        do
        {
          const UInt           subTUIndex             = TUIterator.GetSectionNumber();
          const UInt           subTUAbsPartIdx        = TUIterator.GetAbsPartIdxTU(compID);
          const TComRectangle &tuCompRect             = TUIterator.getRect(compID);
          const UInt           subTUBufferOffset      = tuCompRect.width * tuCompRect.height * subTUIndex;

                TCoeff        *currentCoefficients    = pcCoeffCurr[compID] + subTUBufferOffset;
#if ADAPTIVE_QP_SELECTION
                TCoeff        *currentARLCoefficients = pcArlCoeffCurr[compID] + subTUBufferOffset;
#endif
          const Bool isCrossCPredictionAvailable      =    isChroma(compID)
                                                         && pcCU->getSlice()->getPPS()->getPpsRangeExtension().getCrossComponentPredictionEnabledFlag()
                                                         && (pcCU->getCbf(subTUAbsPartIdx, COMPONENT_Y, uiTrMode) != 0);

          SChar preCalcAlpha = 0;
          const Pel *pLumaResi = m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix( COMPONENT_Y, rTu.getRect( COMPONENT_Y ).x0, rTu.getRect( COMPONENT_Y ).y0 );

          if (isCrossCPredictionAvailable)
          {
            const Bool bUseReconstructedResidualForEstimate = m_pcEncCfg->getUseReconBasedCrossCPredictionEstimate();
            const Pel  *const lumaResidualForEstimate       = bUseReconstructedResidualForEstimate ? pLumaResi                                                     : pcResi->getAddrPix(COMPONENT_Y, tuCompRect.x0, tuCompRect.y0);
            const UInt        lumaResidualStrideForEstimate = bUseReconstructedResidualForEstimate ? m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(COMPONENT_Y) : pcResi->getStride(COMPONENT_Y);

            preCalcAlpha = xCalcCrossComponentPredictionAlpha(TUIterator,
                                                              compID,
                                                              lumaResidualForEstimate,
                                                              pcResi->getAddrPix(compID, tuCompRect.x0, tuCompRect.y0),
                                                              tuCompRect.width,
                                                              tuCompRect.height,
                                                              lumaResidualStrideForEstimate,
                                                              pcResi->getStride(compID));
          }

          const Int transformSkipModesToTest    = checkTransformSkip[compID] ? 2 : 1;
          const Int crossCPredictionModesToTest = (preCalcAlpha != 0)        ? 2 : 1; // preCalcAlpha cannot be anything other than 0 if isCrossCPredictionAvailable is false

          const Bool isOneMode                  = (crossCPredictionModesToTest == 1) && (transformSkipModesToTest == 1);

          for (Int transformSkipModeId = 0; transformSkipModeId < transformSkipModesToTest; transformSkipModeId++)
          {
            pcCU->setTransformSkipPartRange(transformSkipModeId, compID, subTUAbsPartIdx, partIdxesPerSubTU);

            for (Int crossCPredictionModeId = 0; crossCPredictionModeId < crossCPredictionModesToTest; crossCPredictionModeId++)
            {
              const Bool isFirstMode          = (transformSkipModeId == 0) && (crossCPredictionModeId == 0);
              const Bool bUseCrossCPrediction = crossCPredictionModeId != 0;

              m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[ uiDepth ][ CI_QT_TRAFO_ROOT ] );
              m_pcEntropyCoder->resetBits();

              pcCU->setTransformSkipPartRange(transformSkipModeId, compID, subTUAbsPartIdx, partIdxesPerSubTU);
              pcCU->setCrossComponentPredictionAlphaPartRange((bUseCrossCPrediction ? preCalcAlpha : 0), compID, subTUAbsPartIdx, partIdxesPerSubTU );

              if ((compID != COMPONENT_Cr) && ((transformSkipModeId == 1) ? m_pcEncCfg->getUseRDOQTS() : m_pcEncCfg->getUseRDOQ()))
              {
                m_pcEntropyCoder->estimateBit(m_pcTrQuant->m_pcEstBitsSbac, tuCompRect.width, tuCompRect.height, toChannelType(compID));
              }

#if RDOQ_CHROMA_LAMBDA
              m_pcTrQuant->selectLambda(compID);
#endif

              Pel *pcResiCurrComp = m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix(compID, tuCompRect.x0, tuCompRect.y0);
              UInt resiStride     = m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(compID);

              TCoeff bestCoeffComp   [MAX_TU_SIZE*MAX_TU_SIZE];
              Pel    bestResiComp    [MAX_TU_SIZE*MAX_TU_SIZE];

#if ADAPTIVE_QP_SELECTION
              TCoeff bestArlCoeffComp[MAX_TU_SIZE*MAX_TU_SIZE];
#endif
              TCoeff     currAbsSum   = 0;
              UInt       currCompBits = 0;
              Distortion currCompDist = 0;
              Double     currCompCost = 0;
              UInt       nonCoeffBits = 0;
              Distortion nonCoeffDist = 0;
              Double     nonCoeffCost = 0;

              if(!isOneMode && !isFirstMode)
              {
                memcpy(bestCoeffComp,    currentCoefficients,    (sizeof(TCoeff) * tuCompRect.width * tuCompRect.height));
#if ADAPTIVE_QP_SELECTION
                memcpy(bestArlCoeffComp, currentARLCoefficients, (sizeof(TCoeff) * tuCompRect.width * tuCompRect.height));
#endif
                for(Int y = 0; y < tuCompRect.height; y++)
                {
                  memcpy(&bestResiComp[y * tuCompRect.width], (pcResiCurrComp + (y * resiStride)), (sizeof(Pel) * tuCompRect.width));
                }
              }

              if (bUseCrossCPrediction)
              {
                TComTrQuant::crossComponentPrediction(TUIterator,
                                                      compID,
                                                      pLumaResi,
                                                      pcResi->getAddrPix(compID, tuCompRect.x0, tuCompRect.y0),
                                                      crossCPredictedResidualBuffer,
                                                      tuCompRect.width,
                                                      tuCompRect.height,
                                                      m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(COMPONENT_Y),
                                                      pcResi->getStride(compID),
                                                      tuCompRect.width,
                                                      false);

                m_pcTrQuant->transformNxN(TUIterator, compID, crossCPredictedResidualBuffer, tuCompRect.width, currentCoefficients,
#if ADAPTIVE_QP_SELECTION
                                          currentARLCoefficients,
#endif
                                          currAbsSum, cQP);
              }
              else
              {
                m_pcTrQuant->transformNxN(TUIterator, compID, pcResi->getAddrPix( compID, tuCompRect.x0, tuCompRect.y0 ), pcResi->getStride(compID), currentCoefficients,
#if ADAPTIVE_QP_SELECTION
                                          currentARLCoefficients,
#endif
                                          currAbsSum, cQP);
              }

              if(isFirstMode || (currAbsSum == 0))
              {
                if (bUseCrossCPrediction)
                {
                  TComTrQuant::crossComponentPrediction(TUIterator,
                                                        compID,
                                                        pLumaResi,
                                                        m_pTempPel,
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix(compID, tuCompRect.x0, tuCompRect.y0),
                                                        tuCompRect.width,
                                                        tuCompRect.height,
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(COMPONENT_Y),
                                                        tuCompRect.width,
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(compID),
                                                        true);

                  nonCoeffDist = m_pcRdCost->getDistPart( channelBitDepth, m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix( compID, tuCompRect.x0, tuCompRect.y0 ),
                                                          m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride( compID ), pcResi->getAddrPix( compID, tuCompRect.x0, tuCompRect.y0 ),
                                                          pcResi->getStride(compID), tuCompRect.width, tuCompRect.height, compID); // initialized with zero residual distortion
                }
                else
                {
                  nonCoeffDist = m_pcRdCost->getDistPart( channelBitDepth, m_pTempPel, tuCompRect.width, pcResi->getAddrPix( compID, tuCompRect.x0, tuCompRect.y0 ),
                                                          pcResi->getStride(compID), tuCompRect.width, tuCompRect.height, compID); // initialized with zero residual distortion
                }

                m_pcEntropyCoder->encodeQtCbfZero( TUIterator, toChannelType(compID) );

                if ( isCrossCPredictionAvailable )
                {
                  m_pcEntropyCoder->encodeCrossComponentPrediction( TUIterator, compID );
                }

                nonCoeffBits = m_pcEntropyCoder->getNumberOfWrittenBits();
                nonCoeffCost = m_pcRdCost->calcRdCost( nonCoeffBits, nonCoeffDist );
              }

              if((puiZeroDist != NULL) && isFirstMode)
              {
                *puiZeroDist += nonCoeffDist; // initialized with zero residual distortion
              }

              DEBUG_STRING_NEW(sSingleStringTest)

              if( currAbsSum > 0 ) //if non-zero coefficients are present, a residual needs to be derived for further prediction
              {
                if (isFirstMode)
                {
                  m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[ uiDepth ][ CI_QT_TRAFO_ROOT ] );
                  m_pcEntropyCoder->resetBits();
                }

                m_pcEntropyCoder->encodeQtCbf( TUIterator, compID, true );

                if (isCrossCPredictionAvailable)
                {
                  m_pcEntropyCoder->encodeCrossComponentPrediction( TUIterator, compID );
                }

                m_pcEntropyCoder->encodeCoeffNxN( TUIterator, currentCoefficients, compID );
                currCompBits = m_pcEntropyCoder->getNumberOfWrittenBits();

                pcResiCurrComp = m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix( compID, tuCompRect.x0, tuCompRect.y0 );

                m_pcTrQuant->invTransformNxN( TUIterator, compID, pcResiCurrComp, m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(compID), currentCoefficients, cQP DEBUG_STRING_PASS_INTO_OPTIONAL(&sSingleStringTest, (DebugOptionList::DebugString_InvTran.getInt()&debugPredModeMask)) );

                if (bUseCrossCPrediction)
                {
                  TComTrQuant::crossComponentPrediction(TUIterator,
                                                        compID,
                                                        pLumaResi,
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix(compID, tuCompRect.x0, tuCompRect.y0),
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix(compID, tuCompRect.x0, tuCompRect.y0),
                                                        tuCompRect.width,
                                                        tuCompRect.height,
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(COMPONENT_Y),
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(compID     ),
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(compID     ),
                                                        true);
                }

                currCompDist = m_pcRdCost->getDistPart( channelBitDepth, m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix( compID, tuCompRect.x0, tuCompRect.y0 ),
                                                        m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(compID),
                                                        pcResi->getAddrPix( compID, tuCompRect.x0, tuCompRect.y0 ),
                                                        pcResi->getStride(compID),
                                                        tuCompRect.width, tuCompRect.height, compID);

                currCompCost = m_pcRdCost->calcRdCost(currCompBits, currCompDist);
                  
                if (pcCU->isLosslessCoded(0))
                {
                  nonCoeffCost = MAX_DOUBLE;
                }
              }
              else if ((transformSkipModeId == 1) && !bUseCrossCPrediction)
              {
                currCompCost = MAX_DOUBLE;
              }
              else
              {
                currCompBits = nonCoeffBits;
                currCompDist = nonCoeffDist;
                currCompCost = nonCoeffCost;
              }

              // evaluate
              if ((currCompCost < minCost[compID][subTUIndex]) || ((transformSkipModeId == 1) && (currCompCost == minCost[compID][subTUIndex])))
              {
                bestExplicitRdpcmModeUnSplit[compID][subTUIndex] = pcCU->getExplicitRdpcmMode(compID, subTUAbsPartIdx);

                if(isFirstMode) //check for forced null
                {
                  if((nonCoeffCost < currCompCost) || (currAbsSum == 0))
                  {
                    memset(currentCoefficients, 0, (sizeof(TCoeff) * tuCompRect.width * tuCompRect.height));

                    currAbsSum   = 0;
                    currCompBits = nonCoeffBits;
                    currCompDist = nonCoeffDist;
                    currCompCost = nonCoeffCost;
                  }
                }

#if DEBUG_STRING
                if (currAbsSum > 0)
                {
                  DEBUG_STRING_SWAP(sSingleStringComp[compID], sSingleStringTest)
                }
                else
                {
                  sSingleStringComp[compID].clear();
                }
#endif

                uiAbsSum                 [compID][subTUIndex] = currAbsSum;
                uiSingleDistComp         [compID][subTUIndex] = currCompDist;
                minCost                  [compID][subTUIndex] = currCompCost;
                uiBestTransformMode      [compID][subTUIndex] = transformSkipModeId;
                bestCrossCPredictionAlpha[compID][subTUIndex] = (crossCPredictionModeId == 1) ? pcCU->getCrossComponentPredictionAlpha(subTUAbsPartIdx, compID) : 0;

                if (uiAbsSum[compID][subTUIndex] == 0)
                {
                  if (bUseCrossCPrediction)
                  {
                    TComTrQuant::crossComponentPrediction(TUIterator,
                                                          compID,
                                                          pLumaResi,
                                                          m_pTempPel,
                                                          m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix(compID, tuCompRect.x0, tuCompRect.y0),
                                                          tuCompRect.width,
                                                          tuCompRect.height,
                                                          m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(COMPONENT_Y),
                                                          tuCompRect.width,
                                                          m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(compID),
                                                          true);
                  }
                  else
                  {
                    pcResiCurrComp = m_pcQTTempTComYuv[uiQTTempAccessLayer].getAddrPix(compID, tuCompRect.x0, tuCompRect.y0);
                    const UInt uiStride = m_pcQTTempTComYuv[uiQTTempAccessLayer].getStride(compID);
                    for(UInt uiY = 0; uiY < tuCompRect.height; uiY++)
                    {
                      memset(pcResiCurrComp, 0, (sizeof(Pel) * tuCompRect.width));
                      pcResiCurrComp += uiStride;
                    }
                  }
                }
              }
              else
              {
                // reset
                memcpy(currentCoefficients,    bestCoeffComp,    (sizeof(TCoeff) * tuCompRect.width * tuCompRect.height));
#if ADAPTIVE_QP_SELECTION
                memcpy(currentARLCoefficients, bestArlCoeffComp, (sizeof(TCoeff) * tuCompRect.width * tuCompRect.height));
#endif
                for (Int y = 0; y < tuCompRect.height; y++)
                {
                  memcpy((pcResiCurrComp + (y * resiStride)), &bestResiComp[y * tuCompRect.width], (sizeof(Pel) * tuCompRect.width));
                }
              }
            }
          }

          pcCU->setExplicitRdpcmModePartRange            (   bestExplicitRdpcmModeUnSplit[compID][subTUIndex],                            compID, subTUAbsPartIdx, partIdxesPerSubTU);
          pcCU->setTransformSkipPartRange                (   uiBestTransformMode         [compID][subTUIndex],                            compID, subTUAbsPartIdx, partIdxesPerSubTU );
          pcCU->setCbfPartRange                          ((((uiAbsSum                    [compID][subTUIndex] > 0) ? 1 : 0) << uiTrMode), compID, subTUAbsPartIdx, partIdxesPerSubTU );
          pcCU->setCrossComponentPredictionAlphaPartRange(   bestCrossCPredictionAlpha   [compID][subTUIndex],                            compID, subTUAbsPartIdx, partIdxesPerSubTU );
        } while (TUIterator.nextSection(rTu)); //end of sub-TU loop
      } // processing section
    } // component loop

    for(UInt ch = 0; ch < numValidComp; ch++)
    {
      const ComponentID compID = ComponentID(ch);
      if (rTu.ProcessComponentSection(compID) && (rTu.getRect(compID).width != rTu.getRect(compID).height))
      {
        offsetSubTUCBFs(rTu, compID); //the CBFs up to now have been defined for two sub-TUs - shift them down a level and replace with the parent level CBF
      }
    }

    m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[ uiDepth ][ CI_QT_TRAFO_ROOT ] );
    m_pcEntropyCoder->resetBits();

    if( uiLog2TrSize > pcCU->getQuadtreeTULog2MinSizeInCU(uiAbsPartIdx) )
    {
      m_pcEntropyCoder->encodeTransformSubdivFlag( 0, 5 - uiLog2TrSize );
    }

    for(UInt ch = 0; ch < numValidComp; ch++)
    {
      const UInt chOrderChange = ((ch + 1) == numValidComp) ? 0 : (ch + 1);
      const ComponentID compID=ComponentID(chOrderChange);
      if( rTu.ProcessComponentSection(compID) )
      {
        m_pcEntropyCoder->encodeQtCbf( rTu, compID, true );
      }
    }

    for(UInt ch = 0; ch < numValidComp; ch++)
    {
      const ComponentID compID=ComponentID(ch);
      if (rTu.ProcessComponentSection(compID))
      {
        if(isChroma(compID) && (uiAbsSum[COMPONENT_Y][0] != 0))
        {
          m_pcEntropyCoder->encodeCrossComponentPrediction( rTu, compID );
        }

        m_pcEntropyCoder->encodeCoeffNxN( rTu, pcCoeffCurr[compID], compID );
        for (UInt subTUIndex = 0; subTUIndex < 2; subTUIndex++)
        {
          uiSingleDist += uiSingleDistComp[compID][subTUIndex];
        }
      }
    }

    uiSingleBits = m_pcEntropyCoder->getNumberOfWrittenBits();

    dSingleCost = m_pcRdCost->calcRdCost( uiSingleBits, uiSingleDist );
  } // check full

  // code sub-blocks
  if( bCheckSplit )
  {
    if( bCheckFull )
    {
      m_pcRDGoOnSbacCoder->store( m_pppcRDSbacCoder[ uiDepth ][ CI_QT_TRAFO_TEST ] );
      m_pcRDGoOnSbacCoder->load ( m_pppcRDSbacCoder[ uiDepth ][ CI_QT_TRAFO_ROOT ] );
    }
    Distortion uiSubdivDist = 0;
    UInt       uiSubdivBits = 0;
    Double     dSubdivCost = 0.0;

    //save the non-split CBFs in case we need to restore them later

    UInt bestCBF     [MAX_NUM_COMPONENT];
    UInt bestsubTUCBF[MAX_NUM_COMPONENT][2];
    for(UInt ch = 0; ch < numValidComp; ch++)
    {
      const ComponentID compID=ComponentID(ch);

      if (rTu.ProcessComponentSection(compID))
      {
        bestCBF[compID] = pcCU->getCbf(uiAbsPartIdx, compID, uiTrMode);

        const TComRectangle &tuCompRect = rTu.getRect(compID);
        if (tuCompRect.width != tuCompRect.height)
        {
          const UInt partIdxesPerSubTU = rTu.GetAbsPartIdxNumParts(compID) >> 1;

          for (UInt subTU = 0; subTU < 2; subTU++)
          {
            bestsubTUCBF[compID][subTU] = pcCU->getCbf ((uiAbsPartIdx + (subTU * partIdxesPerSubTU)), compID, subTUDepth);
          }
        }
      }
    }


    TComTURecurse tuRecurseChild(rTu, false);
    const UInt uiQPartNumSubdiv = tuRecurseChild.GetAbsPartIdxNumParts();

    DEBUG_STRING_NEW(sSplitString[MAX_NUM_COMPONENT])

    do
    {
      DEBUG_STRING_NEW(childString)
      xEstimateInterResidualQT( pcResi, dSubdivCost, uiSubdivBits, uiSubdivDist, bCheckFull ? NULL : puiZeroDist,  tuRecurseChild DEBUG_STRING_PASS_INTO(childString));
#if DEBUG_STRING
      // split the string by component and append to the relevant output (because decoder decodes in channel order, whereas this search searches by TU-order)
      std::size_t lastPos=0;
      const std::size_t endStrng=childString.find(debug_reorder_data_inter_token[MAX_NUM_COMPONENT], lastPos);
      for(UInt ch = 0; ch < numValidComp; ch++)
      {
        if (lastPos!=std::string::npos && childString.find(debug_reorder_data_inter_token[ch], lastPos)==lastPos)
        {
          lastPos+=strlen(debug_reorder_data_inter_token[ch]); // skip leading string
        }
        std::size_t pos=childString.find(debug_reorder_data_inter_token[ch+1], lastPos);
        if (pos!=std::string::npos && pos>endStrng)
        {
          lastPos=endStrng;
        }
        sSplitString[ch]+=childString.substr(lastPos, (pos==std::string::npos)? std::string::npos : (pos-lastPos) );
        lastPos=pos;
      }
#endif
    } while ( tuRecurseChild.nextSection(rTu) ) ;

    UInt uiCbfAny=0;
    for(UInt ch = 0; ch < numValidComp; ch++)
    {
      UInt uiYUVCbf = 0;
      for( UInt ui = 0; ui < 4; ++ui )
      {
        uiYUVCbf |= pcCU->getCbf( uiAbsPartIdx + ui * uiQPartNumSubdiv, ComponentID(ch),  uiTrMode + 1 );
      }
      UChar *pBase=pcCU->getCbf( ComponentID(ch) );
      const UInt flags=uiYUVCbf << uiTrMode;
      for( UInt ui = 0; ui < 4 * uiQPartNumSubdiv; ++ui )
      {
        pBase[uiAbsPartIdx + ui] |= flags;
      }
      uiCbfAny|=uiYUVCbf;
    }

    m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[ uiDepth ][ CI_QT_TRAFO_ROOT ] );
    m_pcEntropyCoder->resetBits();

    // when compID isn't a channel, code Cbfs:
    xEncodeInterResidualQT( MAX_NUM_COMPONENT, rTu );
    for(UInt ch = 0; ch < numValidComp; ch++)
    {
      xEncodeInterResidualQT( ComponentID(ch), rTu );
    }

    uiSubdivBits = m_pcEntropyCoder->getNumberOfWrittenBits();
    dSubdivCost  = m_pcRdCost->calcRdCost( uiSubdivBits, uiSubdivDist );

    if (!bCheckFull || (uiCbfAny && (dSubdivCost < dSingleCost)))
    {
      rdCost += dSubdivCost;
      ruiBits += uiSubdivBits;
      ruiDist += uiSubdivDist;
#if DEBUG_STRING
      for(UInt ch = 0; ch < numValidComp; ch++)
      {
        DEBUG_STRING_APPEND(sDebug, debug_reorder_data_inter_token[ch])
        DEBUG_STRING_APPEND(sDebug, sSplitString[ch])
      }
#endif
    }
    else
    {
      rdCost  += dSingleCost;
      ruiBits += uiSingleBits;
      ruiDist += uiSingleDist;

      //restore state to unsplit

      pcCU->setTrIdxSubParts( uiTrMode, uiAbsPartIdx, uiDepth );

      for(UInt ch = 0; ch < numValidComp; ch++)
      {
        const ComponentID compID=ComponentID(ch);

        DEBUG_STRING_APPEND(sDebug, debug_reorder_data_inter_token[ch])
        if (rTu.ProcessComponentSection(compID))
        {
          DEBUG_STRING_APPEND(sDebug, sSingleStringComp[compID])

          const Bool splitIntoSubTUs   = rTu.getRect(compID).width != rTu.getRect(compID).height;
          const UInt numberOfSections  = splitIntoSubTUs ? 2 : 1;
          const UInt partIdxesPerSubTU = rTu.GetAbsPartIdxNumParts(compID) >> (splitIntoSubTUs ? 1 : 0);

          for (UInt subTUIndex = 0; subTUIndex < numberOfSections; subTUIndex++)
          {
            const UInt  uisubTUPartIdx = uiAbsPartIdx + (subTUIndex * partIdxesPerSubTU);

            if (splitIntoSubTUs)
            {
              const UChar combinedCBF = (bestsubTUCBF[compID][subTUIndex] << subTUDepth) | (bestCBF[compID] << uiTrMode);
              pcCU->setCbfPartRange(combinedCBF, compID, uisubTUPartIdx, partIdxesPerSubTU);
            }
            else
            {
              pcCU->setCbfPartRange((bestCBF[compID] << uiTrMode), compID, uisubTUPartIdx, partIdxesPerSubTU);
            }

            pcCU->setCrossComponentPredictionAlphaPartRange(bestCrossCPredictionAlpha[compID][subTUIndex], compID, uisubTUPartIdx, partIdxesPerSubTU);
            pcCU->setTransformSkipPartRange(uiBestTransformMode[compID][subTUIndex], compID, uisubTUPartIdx, partIdxesPerSubTU);
            pcCU->setExplicitRdpcmModePartRange(bestExplicitRdpcmModeUnSplit[compID][subTUIndex], compID, uisubTUPartIdx, partIdxesPerSubTU);
          }
        }
      }

      m_pcRDGoOnSbacCoder->load( m_pppcRDSbacCoder[ uiDepth ][ CI_QT_TRAFO_TEST ] );
    }
  }
  else
  {
    rdCost  += dSingleCost;
    ruiBits += uiSingleBits;
    ruiDist += uiSingleDist;
#if DEBUG_STRING
    for(UInt ch = 0; ch < numValidComp; ch++)
    {
      const ComponentID compID=ComponentID(ch);
      DEBUG_STRING_APPEND(sDebug, debug_reorder_data_inter_token[compID])

      if (rTu.ProcessComponentSection(compID))
      {
        DEBUG_STRING_APPEND(sDebug, sSingleStringComp[compID])
      }
    }
#endif
  }
  DEBUG_STRING_APPEND(sDebug, debug_reorder_data_inter_token[MAX_NUM_COMPONENT])
}



Void TEncSearch::xEncodeInterResidualQT( const ComponentID compID, TComTU &rTu )
{
  TComDataCU* pcCU=rTu.getCU();
  const UInt uiAbsPartIdx=rTu.GetAbsPartIdxTU();
  const UInt uiCurrTrMode = rTu.GetTransformDepthRel();
  assert( pcCU->getDepth( 0 ) == pcCU->getDepth( uiAbsPartIdx ) );
  const UInt uiTrMode = pcCU->getTransformIdx( uiAbsPartIdx );

  const Bool bSubdiv = uiCurrTrMode != uiTrMode;

  const UInt uiLog2TrSize = rTu.GetLog2LumaTrSize();

  if (compID==MAX_NUM_COMPONENT)  // we are not processing a channel, instead we always recurse and code the CBFs
  {
    if( uiLog2TrSize <= pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() && uiLog2TrSize > pcCU->getQuadtreeTULog2MinSizeInCU(uiAbsPartIdx) )
    {
      if((pcCU->getSlice()->getSPS()->getQuadtreeTUMaxDepthInter() == 1) && (pcCU->getPartitionSize(uiAbsPartIdx) != SIZE_2Nx2N))
      {
        assert(bSubdiv); // Inferred splitting rule - see derivation and use of interSplitFlag in the specification.
      }
      else
      {
        m_pcEntropyCoder->encodeTransformSubdivFlag( bSubdiv, 5 - uiLog2TrSize );
      }
    }

    assert( !pcCU->isIntra(uiAbsPartIdx) );

    const Bool bFirstCbfOfCU = uiCurrTrMode == 0;

    for (UInt ch=COMPONENT_Cb; ch<pcCU->getPic()->getNumberValidComponents(); ch++)
    {
      const ComponentID compIdInner=ComponentID(ch);
      if( bFirstCbfOfCU || rTu.ProcessingAllQuadrants(compIdInner) )
      {
        if( bFirstCbfOfCU || pcCU->getCbf( uiAbsPartIdx, compIdInner, uiCurrTrMode - 1 ) )
        {
          m_pcEntropyCoder->encodeQtCbf( rTu, compIdInner, !bSubdiv );
        }
      }
      else
      {
        assert( pcCU->getCbf( uiAbsPartIdx, compIdInner, uiCurrTrMode ) == pcCU->getCbf( uiAbsPartIdx, compIdInner, uiCurrTrMode - 1 ) );
      }
    }

    if (!bSubdiv)
    {
      m_pcEntropyCoder->encodeQtCbf( rTu, COMPONENT_Y, true );
    }
  }

  if( !bSubdiv )
  {
    if (compID != MAX_NUM_COMPONENT) // we have already coded the CBFs, so now we code coefficients
    {
      if (rTu.ProcessComponentSection(compID))
      {
        if (isChroma(compID) && (pcCU->getCbf(uiAbsPartIdx, COMPONENT_Y, uiTrMode) != 0))
        {
          m_pcEntropyCoder->encodeCrossComponentPrediction(rTu, compID);
        }

        if (pcCU->getCbf(uiAbsPartIdx, compID, uiTrMode) != 0)
        {
          const UInt uiQTTempAccessLayer = pcCU->getSlice()->getSPS()->getQuadtreeTULog2MaxSize() - uiLog2TrSize;
          TCoeff *pcCoeffCurr = m_ppcQTTempCoeff[compID][uiQTTempAccessLayer] + rTu.getCoefficientOffset(compID);
          m_pcEntropyCoder->encodeCoeffNxN( rTu, pcCoeffCurr, compID );
        }
      }
    }
  }
  else
  {
    if( compID==MAX_NUM_COMPONENT || pcCU->getCbf( uiAbsPartIdx, compID, uiCurrTrMode ) )
    {
      TComTURecurse tuRecurseChild(rTu, false);
      do
      {
        xEncodeInterResidualQT( compID, tuRecurseChild );
      } while (tuRecurseChild.nextSection(rTu));
    }
  }
}




Void TEncSearch::xSetInterResidualQTData( TComYuv* pcResi, Bool bSpatial, TComTU &rTu ) // TODO: turn this into two functions for bSpatial=true and false.
{
  TComDataCU* pcCU=rTu.getCU();
  const UInt uiCurrTrMode=rTu.GetTransformDepthRel();
  const UInt uiAbsPartIdx=rTu.GetAbsPartIdxTU();
  assert( pcCU->getDepth( 0 ) == pcCU->getDepth( uiAbsPartIdx ) );
  const UInt uiTrMode = pcCU->getTransformIdx( uiAbsPartIdx );
  const TComSPS *sps=pcCU->getSlice()->getSPS();

  if( uiCurrTrMode == uiTrMode )
  {
    const UInt uiLog2TrSize = rTu.GetLog2LumaTrSize();
    const UInt uiQTTempAccessLayer = sps->getQuadtreeTULog2MaxSize() - uiLog2TrSize;

    if( bSpatial )
    {
      // Data to be copied is in the spatial domain, i.e., inverse-transformed.

      for(UInt i=0; i<pcResi->getNumberValidComponents(); i++)
      {
        const ComponentID compID=ComponentID(i);
        if (rTu.ProcessComponentSection(compID))
        {
          const TComRectangle &rectCompTU(rTu.getRect(compID));
          m_pcQTTempTComYuv[uiQTTempAccessLayer].copyPartToPartComponentMxN    ( compID, pcResi, rectCompTU );
        }
      }
    }
    else
    {
      for (UInt ch=0; ch < getNumberValidComponents(sps->getChromaFormatIdc()); ch++)
      {
        const ComponentID compID   = ComponentID(ch);
        if (rTu.ProcessComponentSection(compID))
        {
          const TComRectangle &rectCompTU(rTu.getRect(compID));
          const UInt numCoeffInBlock    = rectCompTU.width * rectCompTU.height;
          const UInt offset             = rTu.getCoefficientOffset(compID);
          TCoeff* dest                  = pcCU->getCoeff(compID)                        + offset;
          const TCoeff* src             = m_ppcQTTempCoeff[compID][uiQTTempAccessLayer] + offset;
          ::memcpy( dest, src, sizeof(TCoeff)*numCoeffInBlock );

#if ADAPTIVE_QP_SELECTION
          TCoeff* pcArlCoeffSrc            = m_ppcQTTempArlCoeff[compID][uiQTTempAccessLayer] + offset;
          TCoeff* pcArlCoeffDst            = pcCU->getArlCoeff(compID)                        + offset;
          ::memcpy( pcArlCoeffDst, pcArlCoeffSrc, sizeof( TCoeff ) * numCoeffInBlock );
#endif
        }
      }
    }
  }
  else
  {

    TComTURecurse tuRecurseChild(rTu, false);
    do
    {
      xSetInterResidualQTData( pcResi, bSpatial, tuRecurseChild );
    } while (tuRecurseChild.nextSection(rTu));
  }
}




UInt TEncSearch::xModeBitsIntra( TComDataCU* pcCU, UInt uiMode, UInt uiPartOffset, UInt uiDepth, const ChannelType chType )
{
  // Reload only contexts required for coding intra mode information
  m_pcRDGoOnSbacCoder->loadIntraDirMode( m_pppcRDSbacCoder[uiDepth][CI_CURR_BEST], chType );

  // Temporarily set the intra dir being tested, and only
  // for absPartIdx, since encodeIntraDirModeLuma/Chroma only use
  // the entry at absPartIdx.

  UChar &rIntraDirVal=pcCU->getIntraDir( chType )[uiPartOffset];
  UChar origVal=rIntraDirVal;
  rIntraDirVal = uiMode;
  //pcCU->setIntraDirSubParts ( chType, uiMode, uiPartOffset, uiDepth + uiInitTrDepth );

  m_pcEntropyCoder->resetBits();
  if (isLuma(chType))
  {
    m_pcEntropyCoder->encodeIntraDirModeLuma ( pcCU, uiPartOffset);
  }
  else
  {
    m_pcEntropyCoder->encodeIntraDirModeChroma ( pcCU, uiPartOffset);
  }

  rIntraDirVal = origVal; // restore

  return m_pcEntropyCoder->getNumberOfWrittenBits();
}




UInt TEncSearch::xUpdateCandList( UInt uiMode, Double uiCost, UInt uiFastCandNum, UInt * CandModeList, Double * CandCostList )
{
  UInt i;
  UInt shift=0;

  while ( shift<uiFastCandNum && uiCost<CandCostList[ uiFastCandNum-1-shift ] )
  {
    shift++;
  }

  if( shift!=0 )
  {
    for(i=1; i<shift; i++)
    {
      CandModeList[ uiFastCandNum-i ] = CandModeList[ uiFastCandNum-1-i ];
      CandCostList[ uiFastCandNum-i ] = CandCostList[ uiFastCandNum-1-i ];
    }
    CandModeList[ uiFastCandNum-shift ] = uiMode;
    CandCostList[ uiFastCandNum-shift ] = uiCost;
    return 1;
  }

  return 0;
}





/** add inter-prediction syntax elements for a CU block
 * \param pcCU
 * \param uiQp
 * \param uiTrMode
 * \param ruiBits
 * \returns Void
 */
Void  TEncSearch::xAddSymbolBitsInter( TComDataCU* pcCU, UInt& ruiBits )
{
  if(pcCU->getMergeFlag( 0 ) && pcCU->getPartitionSize( 0 ) == SIZE_2Nx2N && !pcCU->getQtRootCbf( 0 ))
  {
    pcCU->setSkipFlagSubParts( true, 0, pcCU->getDepth(0) );

    m_pcEntropyCoder->resetBits();
    if(pcCU->getSlice()->getPPS()->getTransquantBypassEnableFlag())
    {
      m_pcEntropyCoder->encodeCUTransquantBypassFlag(pcCU, 0, true);
    }
    m_pcEntropyCoder->encodeSkipFlag(pcCU, 0, true);
    m_pcEntropyCoder->encodeMergeIndex(pcCU, 0, true);

    ruiBits += m_pcEntropyCoder->getNumberOfWrittenBits();
  }
  else
  {
    m_pcEntropyCoder->resetBits();

    if(pcCU->getSlice()->getPPS()->getTransquantBypassEnableFlag())
    {
      m_pcEntropyCoder->encodeCUTransquantBypassFlag(pcCU, 0, true);
    }

    m_pcEntropyCoder->encodeSkipFlag ( pcCU, 0, true );
    m_pcEntropyCoder->encodePredMode( pcCU, 0, true );
    m_pcEntropyCoder->encodePartSize( pcCU, 0, pcCU->getDepth(0), true );
    m_pcEntropyCoder->encodePredInfo( pcCU, 0 );

    Bool codeDeltaQp = false;
    Bool codeChromaQpAdj = false;
    m_pcEntropyCoder->encodeCoeff   ( pcCU, 0, pcCU->getDepth(0), codeDeltaQp, codeChromaQpAdj );

    ruiBits += m_pcEntropyCoder->getNumberOfWrittenBits();
  }
}





/**
 * \brief Generate half-sample interpolated block
 *
 * \param pattern Reference picture ROI
 * \param biPred    Flag indicating whether block is for biprediction
 */
Void TEncSearch::xExtDIFUpSamplingH( TComPattern* pattern )
{
  Int width      = pattern->getROIYWidth();
  Int height     = pattern->getROIYHeight();
  Int srcStride  = pattern->getPatternLStride();

  Int intStride = m_filteredBlockTmp[0].getStride(COMPONENT_Y);
  Int dstStride = m_filteredBlock[0][0].getStride(COMPONENT_Y);
  Pel *intPtr;
  Pel *dstPtr;
  Int filterSize = NTAPS_LUMA;
  Int halfFilterSize = (filterSize>>1);
  Pel *srcPtr = pattern->getROIY() - halfFilterSize*srcStride - 1;

  const ChromaFormat chFmt = m_filteredBlock[0][0].getChromaFormat();

  m_if.filterHor(COMPONENT_Y, srcPtr, srcStride, m_filteredBlockTmp[0].getAddr(COMPONENT_Y), intStride, width+1, height+filterSize, 0, false, chFmt, pattern->getBitDepthY());
  m_if.filterHor(COMPONENT_Y, srcPtr, srcStride, m_filteredBlockTmp[2].getAddr(COMPONENT_Y), intStride, width+1, height+filterSize, 2, false, chFmt, pattern->getBitDepthY());

  intPtr = m_filteredBlockTmp[0].getAddr(COMPONENT_Y) + halfFilterSize * intStride + 1;
  dstPtr = m_filteredBlock[0][0].getAddr(COMPONENT_Y);
  m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width+0, height+0, 0, false, true, chFmt, pattern->getBitDepthY());

  intPtr = m_filteredBlockTmp[0].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride + 1;
  dstPtr = m_filteredBlock[2][0].getAddr(COMPONENT_Y);
  m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width+0, height+1, 2, false, true, chFmt, pattern->getBitDepthY());

  intPtr = m_filteredBlockTmp[2].getAddr(COMPONENT_Y) + halfFilterSize * intStride;
  dstPtr = m_filteredBlock[0][2].getAddr(COMPONENT_Y);
  m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width+1, height+0, 0, false, true, chFmt, pattern->getBitDepthY());

  intPtr = m_filteredBlockTmp[2].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
  dstPtr = m_filteredBlock[2][2].getAddr(COMPONENT_Y);
  m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width+1, height+1, 2, false, true, chFmt, pattern->getBitDepthY());
}





/**
 * \brief Generate quarter-sample interpolated blocks
 *
 * \param pattern    Reference picture ROI
 * \param halfPelRef Half-pel mv
 * \param biPred     Flag indicating whether block is for biprediction
 */
Void TEncSearch::xExtDIFUpSamplingQ( TComPattern* pattern, TComMv halfPelRef )
{
  Int width      = pattern->getROIYWidth();
  Int height     = pattern->getROIYHeight();
  Int srcStride  = pattern->getPatternLStride();

  Pel *srcPtr;
  Int intStride = m_filteredBlockTmp[0].getStride(COMPONENT_Y);
  Int dstStride = m_filteredBlock[0][0].getStride(COMPONENT_Y);
  Pel *intPtr;
  Pel *dstPtr;
  Int filterSize = NTAPS_LUMA;

  Int halfFilterSize = (filterSize>>1);

  Int extHeight = (halfPelRef.getVer() == 0) ? height + filterSize : height + filterSize-1;

  const ChromaFormat chFmt = m_filteredBlock[0][0].getChromaFormat();

  // Horizontal filter 1/4
  srcPtr = pattern->getROIY() - halfFilterSize * srcStride - 1;
  intPtr = m_filteredBlockTmp[1].getAddr(COMPONENT_Y);
  if (halfPelRef.getVer() > 0)
  {
    srcPtr += srcStride;
  }
  if (halfPelRef.getHor() >= 0)
  {
    srcPtr += 1;
  }
  m_if.filterHor(COMPONENT_Y, srcPtr, srcStride, intPtr, intStride, width, extHeight, 1, false, chFmt, pattern->getBitDepthY());

  // Horizontal filter 3/4
  srcPtr = pattern->getROIY() - halfFilterSize*srcStride - 1;
  intPtr = m_filteredBlockTmp[3].getAddr(COMPONENT_Y);
  if (halfPelRef.getVer() > 0)
  {
    srcPtr += srcStride;
  }
  if (halfPelRef.getHor() > 0)
  {
    srcPtr += 1;
  }
  m_if.filterHor(COMPONENT_Y, srcPtr, srcStride, intPtr, intStride, width, extHeight, 3, false, chFmt, pattern->getBitDepthY());

  // Generate @ 1,1
  intPtr = m_filteredBlockTmp[1].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
  dstPtr = m_filteredBlock[1][1].getAddr(COMPONENT_Y);
  if (halfPelRef.getVer() == 0)
  {
    intPtr += intStride;
  }
  m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 1, false, true, chFmt, pattern->getBitDepthY());

  // Generate @ 3,1
  intPtr = m_filteredBlockTmp[1].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
  dstPtr = m_filteredBlock[3][1].getAddr(COMPONENT_Y);
  m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 3, false, true, chFmt, pattern->getBitDepthY());

  if (halfPelRef.getVer() != 0)
  {
    // Generate @ 2,1
    intPtr = m_filteredBlockTmp[1].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
    dstPtr = m_filteredBlock[2][1].getAddr(COMPONENT_Y);
    if (halfPelRef.getVer() == 0)
    {
      intPtr += intStride;
    }
    m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 2, false, true, chFmt, pattern->getBitDepthY());

    // Generate @ 2,3
    intPtr = m_filteredBlockTmp[3].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
    dstPtr = m_filteredBlock[2][3].getAddr(COMPONENT_Y);
    if (halfPelRef.getVer() == 0)
    {
      intPtr += intStride;
    }
    m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 2, false, true, chFmt, pattern->getBitDepthY());
  }
  else
  {
    // Generate @ 0,1
    intPtr = m_filteredBlockTmp[1].getAddr(COMPONENT_Y) + halfFilterSize * intStride;
    dstPtr = m_filteredBlock[0][1].getAddr(COMPONENT_Y);
    m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 0, false, true, chFmt, pattern->getBitDepthY());

    // Generate @ 0,3
    intPtr = m_filteredBlockTmp[3].getAddr(COMPONENT_Y) + halfFilterSize * intStride;
    dstPtr = m_filteredBlock[0][3].getAddr(COMPONENT_Y);
    m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 0, false, true, chFmt, pattern->getBitDepthY());
  }

  if (halfPelRef.getHor() != 0)
  {
    // Generate @ 1,2
    intPtr = m_filteredBlockTmp[2].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
    dstPtr = m_filteredBlock[1][2].getAddr(COMPONENT_Y);
    if (halfPelRef.getHor() > 0)
    {
      intPtr += 1;
    }
    if (halfPelRef.getVer() >= 0)
    {
      intPtr += intStride;
    }
    m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 1, false, true, chFmt, pattern->getBitDepthY());

    // Generate @ 3,2
    intPtr = m_filteredBlockTmp[2].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
    dstPtr = m_filteredBlock[3][2].getAddr(COMPONENT_Y);
    if (halfPelRef.getHor() > 0)
    {
      intPtr += 1;
    }
    if (halfPelRef.getVer() > 0)
    {
      intPtr += intStride;
    }
    m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 3, false, true, chFmt, pattern->getBitDepthY());
  }
  else
  {
    // Generate @ 1,0
    intPtr = m_filteredBlockTmp[0].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride + 1;
    dstPtr = m_filteredBlock[1][0].getAddr(COMPONENT_Y);
    if (halfPelRef.getVer() >= 0)
    {
      intPtr += intStride;
    }
    m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 1, false, true, chFmt, pattern->getBitDepthY());

    // Generate @ 3,0
    intPtr = m_filteredBlockTmp[0].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride + 1;
    dstPtr = m_filteredBlock[3][0].getAddr(COMPONENT_Y);
    if (halfPelRef.getVer() > 0)
    {
      intPtr += intStride;
    }
    m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 3, false, true, chFmt, pattern->getBitDepthY());
  }

  // Generate @ 1,3
  intPtr = m_filteredBlockTmp[3].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
  dstPtr = m_filteredBlock[1][3].getAddr(COMPONENT_Y);
  if (halfPelRef.getVer() == 0)
  {
    intPtr += intStride;
  }
  m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 1, false, true, chFmt, pattern->getBitDepthY());

  // Generate @ 3,3
  intPtr = m_filteredBlockTmp[3].getAddr(COMPONENT_Y) + (halfFilterSize-1) * intStride;
  dstPtr = m_filteredBlock[3][3].getAddr(COMPONENT_Y);
  m_if.filterVer(COMPONENT_Y, intPtr, intStride, dstPtr, dstStride, width, height, 3, false, true, chFmt, pattern->getBitDepthY());
}





//! set wp tables
Void  TEncSearch::setWpScalingDistParam( TComDataCU* pcCU, Int iRefIdx, RefPicList eRefPicListCur )
{
  if ( iRefIdx<0 )
  {
    m_cDistParam.bApplyWeight = false;
    return;
  }

  TComSlice       *pcSlice  = pcCU->getSlice();
  WPScalingParam  *wp0 , *wp1;

  m_cDistParam.bApplyWeight = ( pcSlice->getSliceType()==P_SLICE && pcSlice->testWeightPred() ) || ( pcSlice->getSliceType()==B_SLICE && pcSlice->testWeightBiPred() ) ;

  if ( !m_cDistParam.bApplyWeight )
  {
    return;
  }

  Int iRefIdx0 = ( eRefPicListCur == REF_PIC_LIST_0 ) ? iRefIdx : (-1);
  Int iRefIdx1 = ( eRefPicListCur == REF_PIC_LIST_1 ) ? iRefIdx : (-1);

  getWpScaling( pcCU, iRefIdx0, iRefIdx1, wp0 , wp1 );

  if ( iRefIdx0 < 0 )
  {
    wp0 = NULL;
  }
  if ( iRefIdx1 < 0 )
  {
    wp1 = NULL;
  }

  m_cDistParam.wpCur  = NULL;

  if ( eRefPicListCur == REF_PIC_LIST_0 )
  {
    m_cDistParam.wpCur = wp0;
  }
  else
  {
    m_cDistParam.wpCur = wp1;
  }
}



//! \}
