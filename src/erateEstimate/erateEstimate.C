
/******************************************************************************
 *
 *  This file is part of canu, a software program that assembles whole-genome
 *  sequencing reads into contigs.
 *
 *  This software is based on:
 *    'Celera Assembler' (http://wgs-assembler.sourceforge.net)
 *    the 'kmer package' (http://kmer.sourceforge.net)
 *  both originally distributed by Applera Corporation under the GNU General
 *  Public License, version 2.
 *
 *  Canu branched from Celera Assembler at its revision 4587.
 *  Canu branched from the kmer project at its revision 1994.
 *
 *  This file is derived from:
 *
 *    src/AS_BAT/erate-estimate.C
 *
 *  Modifications by:
 *
 *    Brian P. Walenz from 2014-OCT-21 to 2015-JUL-29
 *      are Copyright 2014-2015 Battelle National Biodefense Institute, and
 *      are subject to the BSD 3-Clause License
 *
 *    Brian P. Walenz beginning on 2015-DEC-07
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "AS_global.H"
#include "sqStore.H"
#include "ovStore.H"

#include "memoryMappedFile.H"

#include "intervalList.H"

#include <map>
#include <set>
#include <list>
#include <vector>
#include <algorithm>

using namespace std;

#ifndef BROKEN_CLANG_OpenMP
#include <omp.h>
#endif

#define ERATE_TOLERANCE 0.03

uint32 blockSize = 1000;

class readErrorEstimate {
public:
  readErrorEstimate() {
    seqLen = 0;

    errorMeanS      = NULL;  //  Sum of error up to this point
    //errorStdDev     = NULL;
    //errorConfidence = NULL;
    errorMeanU      = NULL;  //  Updated error estimate for this point
  };

  ~readErrorEstimate() {
    delete [] errorMeanS;
    //delete [] errorStdDev;
    //delete [] errorConfidence;
    delete [] errorMeanU;
  };

  uint64     initialize(sqRead *read) {
    //uint32      lid = read->sqRead_getLibraryIID();
    //sqLibrary  *lb  = seqStore->sqStore_getLibrary(lid);

    seqLen = read->sqRead_sequenceLength();

    errorMeanS      = new uint32 [seqLen + 1];
    //errorStdDev     = new double [seqLen + 1];
    //errorConfidence = new double [seqLen + 1];
    errorMeanU      = new uint16 [seqLen + 1];

#if 0
    uint32  err40 = AS_OVS_encodeEvalue(0.40);

    errorMeanS[0] = err40;
    errorMeanU[0] = 0;

    //  instead of this loop, we can build one full (0-max_read_len) array,
    //  then copy the start N elements to each errorMeanS

    for (uint32 ii=1; ii<seqLen; ii++) {
      errorMeanS[ii] = errorMeanS[ii-1] + err40;
      errorMeanU[ii] = 0;
    }
#endif

#if 0
    //memset(errorMeanS, 0xff, sizeof(uint32) * (readProfile[iid].seqLen + 1));
    //memset(errorMeanU, 0x00, sizeof(uint16) * (readProfile[iid].seqLen + 1));
#endif

    return(sizeof(uint16) * (seqLen + 1 + seqLen + 1) + sizeof(readErrorEstimate));
  };

  uint32     seqLen;

  uint32    *errorMeanS;
  //double    *errorStdDev;
  //double    *errorConfidence;
  uint16    *errorMeanU;
};


class ESToverlap {
public:
  uint32   a_iid      : 23;
  uint32   b_iid_hi   :  9;  //  32 bits

  uint32   b_iid_lo   : 14;
  int32    a_hang     : 17;  //  31 bits

  int32    b_hang     : 17;
  uint32   erate      : 12;
  uint32   flipped    : 1;
  uint32   discarded  : 1;   //  31 bits

  void     populate(ovOverlap &ovl) {
    a_iid     =  ovl.a_iid;
    b_iid_hi  = (ovl.b_iid >> 14) & 0x000001ff;
    b_iid_lo  = (ovl.b_iid >>  0) & 0x00003fff;
    a_hang    =  ovl.a_hang();
    b_hang    =  ovl.b_hang();
    erate     =  ovl.evalue();
    flipped   =  ovl.flipped();
    discarded =  false;

    assert(ovl.a_iid      ==   a_iid);
    assert(ovl.b_iid      == ((b_iid_hi << 14) | (b_iid_lo)));
    assert(ovl.a_hang()   ==   a_hang);
    assert(ovl.b_hang()   ==   b_hang);
    assert(ovl.erate()    ==   erate);
    assert(ovl.flipped()  ==   flipped);
  };

#if 0
  void     populateOBT(ovOverlap &obt, readErrorEstimate *readProfile, uint32 iidMin) {
    obt.a_iid              =  a_iid;
    obt.b_iid              = (b_iid_hi << 14) | (b_iid_lo);

    int32 seqLenA = readProfile[obt.a_iid - iidMin].seqLen;
    int32 seqLenB = readProfile[obt.b_iid - iidMin].seqLen;

    //  Swiped from AS_OVS_overlap.C

    uint32  abgn    = (a_hang < 0) ? (0)                : (a_hang);
    uint32  aend    = (b_hang < 0) ? (seqLenA + b_hang) : (seqLenA);
    uint32  bbgn    = (a_hang < 0) ? (-a_hang)          : (0);
    uint32  bend    = (b_hang < 0) ? (seqLenB)          : (seqLenB - b_hang);

    obt.dat.obt.type     =  AS_OVS_TYPE_OBT;

    obt.dat.obt.a_beg    =  abgn;
    obt.dat.obt.a_end    =  aend;
    obt.dat.obt.b_beg    =  bbgn;
    obt.dat.obt.b_end_hi = (bend >> 9) & 0x00003fff;
    obt.dat.obt.b_end_lo = (bend)      & 0x000001ff;

    obt.dat.obt.fwd      = (flipped == false);

    obt.dat.obt.erate    =  erate;
  };
#endif
};


class ESToverlapSpan {
public:
  ESToverlapSpan(ESToverlap& ovl, readErrorEstimate *readProfile, uint32 iidMin) {
    a_iid              =  ovl.a_iid;
    b_iid              = (ovl.b_iid_hi << 14) | (ovl.b_iid_lo);

    int32 seqLenA = readProfile[a_iid - iidMin].seqLen;
    int32 seqLenB = readProfile[b_iid - iidMin].seqLen;

    //  Swiped from AS_OVS_overlap.C

    uint32  abgn    = (ovl.a_hang < 0) ? (0)                    : (ovl.a_hang);
    uint32  aend    = (ovl.b_hang < 0) ? (seqLenA + ovl.b_hang) : (seqLenA);
    uint32  bbgn    = (ovl.a_hang < 0) ? (-ovl.a_hang)          : (0);
    uint32  bend    = (ovl.b_hang < 0) ? (seqLenB)              : (seqLenB - ovl.b_hang);

    a_beg    = abgn;
    a_end    = aend;
    b_beg    = bbgn;
    b_end    = bend;

    fwd      = (ovl.flipped == false);

    erate    =  ovl.erate;
  };

  uint32  a_iid;
  uint32  b_iid;

  uint32  a_beg;
  uint32  a_end;
  uint32  b_beg;
  uint32  b_end;

  uint32  fwd;
  uint32  erate;
};







void
saveProfile(uint32             iid,
            uint32             iter,
            readErrorEstimate *readProfile) {
  char    N[FILENAME_MAX];
  snprintf(N, FILENAME_MAX, "erate-%08u-%02u.dat", iid, iter);

  FILE   *F = AS_UTL_openOutputFile(N);

  for (uint32 pp=0; pp<readProfile[iid].seqLen; pp++)
    fprintf(F, "" F_U32 " %7.4f\n", pp, AS_OVS_decodeEvalue(readProfile[iid].errorMeanS[pp]));

  AS_UTL_closeFile(F, N);

  FILE *P = popen("gnuplot ", "w");
  fprintf(P, "set terminal png\n");
  fprintf(P, "set output   'erate-%08d-%02u.png'\n", iid, iter);
  fprintf(P, "plot [] [0.00:0.25] 'erate-%08d-%02u.dat' using 1:2 with lines\n", iid, iter);
  pclose(P);
}




double
computeEstimatedErate(uint32 iidMin, ESToverlapSpan &ovl, readErrorEstimate *readProfile) {
  uint64  estErrorA = 0;
  uint64  estErrorB = 0;
  int32   olapLen   = 0;

  uint32  ab = ovl.a_beg;
  uint32  ae = ovl.a_end;

  assert(ab < ae);

#if 0
  for (uint32 xx=ab; xx <= ae; xx++)
    estErrorA += readProfile[obt.a_iid  - iidMin].errorMean[xx];
  estErrorA /= (ae - ab);
#else
  estErrorA = ((readProfile[ovl.a_iid  - iidMin].errorMeanS[ae]) -
               (readProfile[ovl.a_iid  - iidMin].errorMeanS[ab]));
#endif

  uint32  bb = ovl.b_beg;
  uint32  be = ovl.b_end;

  assert(bb < be);

#if 0
  for (uint32 xx=bb; xx <= be; xx++)
    estErrorB += readProfile[obt.b_iid - iidMin].errorMean[xx];
  estErrorB /= (be - bb);
#else
  estErrorB = ((readProfile[ovl.b_iid  - iidMin].errorMeanS[be]) -
               (readProfile[ovl.b_iid  - iidMin].errorMeanS[bb]));
#endif

  return(AS_OVS_decodeEvalue((estErrorA / 2) + (estErrorB / 2)));
}



void
recomputeErrorProfile(sqStore           *seqStore,
                      uint32             iidMin,
                      uint32             numIIDs,
                      uint64            *overlapIndex,
                      ESToverlap        *overlaps,
                      readErrorEstimate *readProfile,
                      uint32             iter) {
  uint64      nDiscarded   = 0;
  uint64      nDiscard     = 0;
  uint64      nRemain      = 0;

  fprintf(stderr, "Processing from IID " F_U32 " to " F_U32 " out of " F_U32 " reads, iteration " F_U32 ".\n",
          iidMin,
          iidMin + numIIDs,
          seqStore->sqStore_getNumReads(),
          iter);

#pragma omp parallel for schedule(dynamic, blockSize)
  for (uint32 iid=0; iid<numIIDs; iid++) {
    if (readProfile[iid].seqLen == 0)
      //  Deleted read.
      continue;

    //  Build a list of the overlap intervals with their error rate.  Unlike the initial estimates,
    //  we are allowed to skip previously discarded overlaps, and we need to compute estimates and
    //  discard high error overlaps.

    intervalList<uint32,double>   eRateList;

    for (uint64 oo=overlapIndex[iid]; oo<overlapIndex[iid+1]; oo++) {
      if (overlaps[oo].discarded == true) {
        nDiscarded++;
        continue;
      }

      //overlaps[oo].populateOBT(obt, readProfile, iidMin);
      ESToverlapSpan  ovl(overlaps[oo], readProfile, iidMin);

      assert(ovl.a_iid == iid + iidMin);
      assert(ovl.a_beg <= ovl.a_end);

      //  Compute the expected erate for this overlap based on our estimated error in both reads,
      //  and filter the overlap if it is higher than this.

      double erate    = AS_OVS_decodeEvalue(ovl.erate);

      if (iter > 0) {
        double estError = computeEstimatedErate(iidMin, ovl, readProfile);

        if (estError + ERATE_TOLERANCE < erate) {
          overlaps[oo].discarded = true;

          nDiscard++;
          continue;
        }
      }

      //  Otherwise, add it to the list of intervals.

      nRemain++;

      eRateList.add(ovl.a_beg, ovl.a_end - ovl.a_beg, erate / 2);
    }

    //  Convert the list to a sum of error rate per base

    intervalList<uint32,double>   eRateMap(eRateList);

    //  Unpack the list into an array of mean error rate per base

    memset(readProfile[iid].errorMeanU, 0, sizeof(uint16) * (readProfile[iid].seqLen + 1));

    for (uint32 ii=0; ii<eRateMap.numberOfIntervals(); ii++) {
      double eVal = (eRateMap.depth(ii) > 0) ? (eRateMap.value(ii) / eRateMap.depth(ii)) : 0;

      assert(0.0 <= eVal);
      assert(eVal <= 1.0);

      assert(eRateMap.hi(ii) <= readProfile[iid].seqLen);

      uint16  eEnc = AS_OVS_encodeEvalue(eVal);

      for (uint32 pp=eRateMap.lo(ii); pp < eRateMap.hi(ii); pp++)
        readProfile[iid].errorMeanU[pp] = eEnc;
    }

    //  Keep users entertained.

    if ((iid % 1000) == 0)
      fprintf(stderr, "IID " F_U32 "\r", iid);
  }

  //  All new estimates are computed.  Convert the array of mean error per base into an array of
  //  summed error per base

  for (uint32 iid=0; iid<numIIDs; iid++) {
    if (readProfile[iid].seqLen == 0)
      continue;

    readProfile[iid].errorMeanS[0] = readProfile[iid].errorMeanU[0];

    for (uint32 ii=1; ii<readProfile[iid].seqLen; ii++)
      readProfile[iid].errorMeanS[ii] = readProfile[iid].errorMeanS[ii-1] + readProfile[iid].errorMeanU[ii];
  }

  //  Report stats.

  fprintf(stderr, "\n");
  fprintf(stderr, "nDiscarded " F_U64 " (in previous iterations)\n", nDiscarded);
  fprintf(stderr, "nDiscard   " F_U64 " (in this iteration)\n", nDiscard);
  fprintf(stderr, "nRemain    " F_U64 "\n", nRemain);
}







void
outputOverlaps(sqStore           *seqStore,
               uint32             iidMin,
               uint32             numIIDs,
               char              *ovlStoreName,
               uint64            *UNUSED(overlapIndex),
               ESToverlap        *overlaps,
               readErrorEstimate *UNUSED(readProfile),
               char              *outputName) {
  uint64      nDiscarded   = 0;
  uint64      nRemain      = 0;

  //  Open the original and output stores.  We copy overlaps from the original to the copy, instead
  //  of recreating overlaps from our cache.  The cache doesn't have all the overlap information.

  ovStore        *inpStore = new ovStore(ovlStoreName, seqStore);
  ovStoreWriter  *outStore = new ovStoreWriter(outputName, seqStore);

  uint64    numOvls = inpStore->numOverlapsInRange();

  fprintf(stderr, "Processing from IID " F_U32 " to " F_U32 " out of " F_U32 " reads.\n",
          iidMin,
          iidMin + numIIDs,
          seqStore->sqStore_getNumReads());

  //  Can't thread.  This does sequential output.  Plus, it doesn't compute anything.

  //  Overlaps in the store and those in the list should be in lock-step.  We can just
  //  walk down each.

  uint32            ovlLen = 100000000;
  ovOverlap        *ovl = ovOverlap::allocateOverlaps(seqStore, ovlLen);

  for (uint64 no=0; no<numOvls; ) {
    uint64 nLoad  = inpStore->loadBlockOfOverlaps(ovl, ovlLen);

    assert(nLoad > 0);

    for (uint32 xx=0; xx<nLoad; xx++, no++) {
      uint32  a_iid =   overlaps[no].a_iid;
      uint32  b_iid = ((overlaps[no].b_iid_hi << 14) | (overlaps[no].b_iid_lo));

      assert(ovl[xx].a_iid == a_iid);
      assert(ovl[xx].b_iid == b_iid);;

      if (overlaps[no].discarded == true) {
        nDiscarded++;

      } else {
        outStore->writeOverlap(ovl + xx);
        nRemain++;
      }

      if ((no & 0x000fffff) == 0)
        fprintf(stderr, "  overlap %10" F_U64P " %8" F_U32P "-%8" F_U32P "\r", no, a_iid, b_iid);
    }
  }

  delete [] ovl;



#if 0
  for (uint32 iid=0; iid<numIIDs; iid++) {
    if (readProfile[iid].seqLen == 0)
      //  Deleted read.
      continue;

    for (uint64 oo=overlapIndex[iid]; oo<overlapIndex[iid+1]; oo++) {
      ovOverlap  obt;

      if (overlaps[oo].discarded == true) {
        continue;
      }


      //  Emit the overlap

      overlaps[oo].populateOVL(obt);

      outStore->writeOverlap(&obt);
    }

    if ((iid % 1000) == 0)
      fprintf(stderr, "IID " F_U32 "\r", iid);
  }
#endif

  delete outStore;
  delete inpStore;

  fprintf(stderr, "\n");
  fprintf(stderr, "nDiscarded " F_U64 " (in previous iterations)\n", nDiscarded);
  fprintf(stderr, "nRemain    " F_U64 "\n", nRemain);
}








int
main(int argc, char **argv) {
  char             *seqName        = 0L;
  sqStore          *seqStore       = 0L;
  char             *ovlStoreName   = 0L;
  ovStore          *ovlStore       = 0L;

  char             *ovlCacheName   = 0L;

  uint32            errorRate      = AS_OVS_encodeEvalue(0.015);
  double            errorLimit     = 2.5;

  char             *outputPrefix  = NULL;
  char              logName[FILENAME_MAX] = {0};
  char              sumName[FILENAME_MAX] = {0};
  FILE             *logFile = 0L;
  FILE             *sumFile = 0L;

  argc = AS_configure(argc, argv);

  uint32            minEvidenceOverlap  = 40;
  uint32            minEvidenceCoverage = 1;

  uint32            iidMin   = UINT32_MAX;
  uint32            iidMax   = UINT32_MAX;
  uint32            partNum  = 0;
  uint32            partMax  = 1;

  uint32            minLen   = 0;
  double            minErate = 0;

  int arg=1;
  int err=0;

  while (arg < argc) {
    if        (strcmp(argv[arg], "-S") == 0) {
      seqName = argv[++arg];

    } else if (strcmp(argv[arg], "-O") == 0) {
      ovlStoreName = argv[++arg];

    } else if (strcmp(argv[arg], "-C") == 0) {
      ovlCacheName = argv[++arg];

    } else if (strcmp(argv[arg], "-b") == 0) {
      iidMin  = atoi(argv[++arg]);
      partNum = 0;
      partMax = 0;

    } else if (strcmp(argv[arg], "-e") == 0) {
      iidMax = atoi(argv[++arg]);
      partNum = 0;
      partMax = 0;

    } else if (strcmp(argv[arg], "-p") == 0) {
      partNum = atoi(argv[++arg]) - 1;
      partMax = atoi(argv[++arg]);

    } else if (strcmp(argv[arg], "-L") == 0) {
      //minLen = atoi(argv[++arg]);

    } else if (strcmp(argv[arg], "-E") == 0) {
      //minErate = atof(argv[++arg]);

    } else {
      fprintf(stderr, "ERROR: unknown option '%s'\n", argv[arg]);
      err++;
    }

    arg++;
  }
  if (err) {
    exit(1);
  }

  //  Can't use dynamic threads, screws up the thread-local memory allocations.

  omp_set_dynamic(false);

  //  Open sequence store

  fprintf(stderr, "Opening '%s'\n", seqName);
  seqStore = sqStore::sqStore_open(seqName);

  //  Compute what to compute.

  if (partNum < partMax) {
    uint32  nf = seqStore->sqStore_getNumReads();

    iidMin = (partNum + 0) * nf / partMax + 1;
    iidMax = (partNum + 1) * nf / partMax;

    if (partNum + 1 == partMax)
      iidMax = nf;
  }

  if (iidMin == UINT32_MAX)
    iidMin = 1;

  if (iidMax == UINT32_MAX)
    iidMax = seqStore->sqStore_getNumReads();

  uint32    numIIDs = (iidMax - iidMin + 1);

  fprintf(stderr, "  iidMin  = %9u\n", iidMin);
  fprintf(stderr, "  iidMax  = %9u numReads = %9u\n", iidMax, seqStore->sqStore_getNumReads());
  fprintf(stderr, "  partNum = %9u\n", partNum);
  fprintf(stderr, "  partMax = %9u\n", partMax);

  //fprintf(stderr, "ovOverlap " F_U64 "\n", sizeof(ovOverlap));
  //fprintf(stderr, "ESToverlap " F_U64 "\n", sizeof(ESToverlap));

  //  Load read metadata, clear ranges, read lengths, and deleted status.

  fprintf(stderr, "Initializing profiles\n");

  uint64              readProfileSize = 0;
  readErrorEstimate  *readProfile     = new readErrorEstimate [numIIDs];

  for (uint32 iid=0; iid<numIIDs; iid++) {
    readProfileSize += readProfile[iid].initialize(seqStore->sqStore_getRead(iid + iidMin));

    if ((iid % 10000) == 0)
      fprintf(stderr, "  " F_U32 " reads\r", iid);
  }

  fprintf(stderr, "  " F_U32 " reads\n", numIIDs);
  fprintf(stderr, "  " F_U64 " GB\n", readProfileSize >> 30);

  //  Open overlap stores

  fprintf(stderr, "Opening '%s'\n", ovlStoreName);
  ovlStore = new ovStore(ovlStoreName, seqStore);

  fprintf(stderr, "Finding number of overlaps\n");

  ovlStore->setRange(iidMin, iidMax);

  uint64    numOvls = ovlStore->numOverlapsInRange();

  uint64      *overlapIndex = new uint64     [numIIDs + 1];
  uint32       bgn = 0;
  uint32       end = 0;
  uint32      *overlapLen   = ovlStore->numOverlapsPerRead();

  overlapIndex[0] = 0;

  for (uint32 iid=0; iid<numIIDs; iid++)
    overlapIndex[iid+1] = overlapIndex[iid] + overlapLen[iid+1];

  assert(overlapIndex[numIIDs] == numOvls);

  delete [] overlapLen;

  //  Load overlaps.

  fprintf(stderr, "Loading overlaps\n");
  fprintf(stderr, "  number   " F_U64 " overlaps\n",           numOvls);
  fprintf(stderr, "  index    " F_U64 " GB\n",                 (sizeof(uint64)     * (uint64)numIIDs) >> 30);
  fprintf(stderr, "  overlaps " F_U64 " GB (previous size)\n", (sizeof(ovOverlap)  *         numOvls) >> 30);
  fprintf(stderr, "  overlaps " F_U64 " GB\n",                 (sizeof(ESToverlap) *         numOvls) >> 30);

  ESToverlap       *overlaps     = NULL;
  memoryMappedFile *overlapsMMF  = NULL;

  if (AS_UTL_fileExists(ovlCacheName, false, false)) {
    fprintf(stderr, "  cache '%s' detected, load averted\n", ovlCacheName);

    overlapsMMF = new memoryMappedFile(ovlCacheName);
    overlaps    = (ESToverlap *)overlapsMMF->get(0);

  } else {
    FILE             *ESTcache     = NULL;
    uint32            ovlLen = 100000000;
    ovOverlap        *ovl = ovOverlap::allocateOverlaps(seqStore, ovlLen);

    overlaps       = new ESToverlap [numOvls];

    if (ovlCacheName)
      ESTcache = AS_UTL_openOutputFile(ovlCacheName);

    for (uint64 no=0; no<numOvls; ) {
      uint64 nLoad  = ovlStore->loadBlockOfOverlaps(ovl, ovlLen);

      for (uint32 xx=0; xx<nLoad; xx++)
        overlaps[no++].populate(ovl[xx]);

      if (ESTcache)
        fwrite(overlaps + no - nLoad, sizeof(ESToverlap), nLoad, ESTcache);

      fprintf(stderr, "  loading overlaps: " F_U64 " out of " F_U64 " (%.4f%%)\r",
              no, numOvls, 100.0 * no / numOvls);
    }

    delete [] ovl;

    AS_UTL_closeFile(ESTcache, ovlCacheName);

    fprintf(stderr, "\n");
    fprintf(stderr, "  loaded and cached " F_U64 " overlaps.\n", numOvls);
  }

  delete ovlStore;
  ovlStore   = NULL;

  //  Allocate space for the result.

  double     *erate5 = new double [numIIDs];
  double     *erate3 = new double [numIIDs];

  //  Compute the initial read profile.

#if 0
  computeInitialErrorProfile(seqStore, iidMin, numIIDs,
                             overlapIndex,
                             overlaps,
                             readProfile);
#endif

  //  Recompute, using the existing profile to weed out probably false overlaps.

  for (uint32 ii=0; ii<4; ii++)
    recomputeErrorProfile(seqStore, iidMin, numIIDs,
                          overlapIndex,
                          overlaps,
                          readProfile,
                          ii);


  outputOverlaps(seqStore, iidMin, numIIDs,
                 ovlStoreName,
                 overlapIndex,
                 overlaps,
                 readProfile,
                 "TEST.ovlStore");

  if (overlapsMMF) {
    delete    overlapsMMF;
  } else {
    delete [] overlaps;
  }

  exit(0);
}
