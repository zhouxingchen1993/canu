
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
 *  Modifications by:
 *
 *    Brian P. Walenz from 2003-SEP-08 to 2003-OCT-20
 *      are Copyright 2003 Applera Corporation, and
 *      are subject to the GNU General Public License version 2
 *
 *    Brian P. Walenz on 2004-APR-12
 *      are Copyright 2004 Brian P. Walenz, and
 *      are subject to the GNU General Public License version 2
 *
 *    Brian P. Walenz from 2005-MAR-20 to 2014-APR-11
 *      are Copyright 2005,2007,2010-2014 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "existDB.H"
#warning YUCK RELATIVE INCLUDE OF libmeryl.H
#include "../libmeryl.H"
#include "speedCounter.H"


bool
existDB::createFromMeryl(char const  *prefix,
                         uint32       merSize,
                         uint32       lo,
                         uint32       hi,
                         uint32       flags) {

  merylStreamReader *M = new merylStreamReader(prefix);

  bool               beVerbose = false;

  _hashTable  = 0L;
  _buckets    = 0L;
  _counts     = 0L;

  _merSizeInBases        = M->merSize();

  if (merSize != _merSizeInBases) {
    fprintf(stderr, "createFromMeryl()-- ERROR: requested merSize ("F_U32") is different than merSize in meryl database ("F_U32").\n",
            merSize, _merSizeInBases);
    exit(1);
  }

  //  We can set this exactly, but not memory optimal (see meryl/estimate.C:optimalNumberOfBuckets()).
  //  Instead, we just blindly use whatever meryl used.
  //
  uint32 tblBits = M->prefixSize();

  //  But it is faster to reset to this.  Might use 2x the memory.
  //uint32 tblBits = logBaseTwo64(M->numberOfDistinctMers() + 1);

  _shift1      = 2 * _merSizeInBases - tblBits;
  _shift2      = _shift1 / 2;
  _mask1       = uint64MASK(tblBits);
  _mask2       = uint64MASK(_shift1);

  _hshWidth    = uint32ZERO;
  _chkWidth    = 2 * _merSizeInBases - tblBits;
  _cntWidth    = 16;

  _numMers     = uint64ZERO;

  uint64  tableSizeInEntries = uint64ONE << tblBits;
  uint64 *countingTable      = new uint64 [tableSizeInEntries + 1];

  if (beVerbose) {
    fprintf(stderr, "createFromMeryl()-- tableSizeInEntries   "F_U64"\n", tableSizeInEntries);
    fprintf(stderr, "createFromMeryl()-- count range          "F_U32"-"F_U32"\n", lo, hi);
  }

  for (uint64 i=tableSizeInEntries+1; i--; )
    countingTable[i] = 0;

  _isCanonical = flags & existDBcanonical;
  _isForward   = flags & existDBforward;

  if (beVerbose) {
    fprintf(stderr, "createFromMeryl()-- canonical            %c\n", (_isCanonical) ? 'T' : 'F');
    fprintf(stderr, "createFromMeryl()-- forward              %c\n", (_isForward)   ? 'T' : 'F');
  }

  assert(_isCanonical + _isForward == 1);

  //  1) Count bucket sizes
  //     While we don't know the bucket sizes right now, but we do know
  //     how many buckets and how many mers.
  //
  //  Because we could be inserting both forward and reverse, we can't
  //  really move the direction testing outside the loop, unless we
  //  want to do two iterations over M.
  //
  speedCounter  *C = new speedCounter("    %7.2f Mmers -- %5.2f Mmers/second\r", 1000000.0, 0x1fffff, beVerbose);

  while (M->nextMer()) {
    if ((lo <= M->theCount()) && (M->theCount() <= hi)) {
      if (_isForward) {
        countingTable[ HASH(M->theFMer()) ]++;
        _numMers++;
      }

      if (_isCanonical) {
        kMer  r = M->theFMer();
        r.reverseComplement();

        if (M->theFMer() < r)
          countingTable[ HASH(M->theFMer()) ]++;
        else
          countingTable[ HASH(r) ]++;
        _numMers++;
      }

      C->tick();
    }
  }

  if (beVerbose)
    fprintf(stderr, "createFromMeryl()-- Found " F_U64 " mers between count of " F_U32 " and " F_U32 "\n",
            _numMers, lo, hi);

  delete C;
  delete M;

  if (_compressedHash) {
    _hshWidth = 1;
    while ((_numMers+1) > (uint64ONE << _hshWidth))
      _hshWidth++;
  }

  //  2) Allocate hash table, mer storage buckets
  //
  _hashTableWords = tableSizeInEntries + 2;
  if (_compressedHash)
    _hashTableWords = _hashTableWords * _hshWidth / 64 + 1;

  _bucketsWords = _numMers + 2;
  if (_compressedBucket)
    _bucketsWords = _bucketsWords * _chkWidth / 64 + 1;

  _countsWords = _numMers + 2;
  if (_compressedCounts)
    _countsWords = _countsWords * _cntWidth / 64 + 1;

  if (beVerbose) {
    fprintf(stderr, "existDB::createFromMeryl()-- hashTable is "F_U64"MB\n", _hashTableWords >> 17);
    fprintf(stderr, "existDB::createFromMeryl()-- buckets is "F_U64"MB\n", _bucketsWords >> 17);
    if (flags & existDBcounts)
      fprintf(stderr, "existDB::createFromMeryl()-- counts is "F_U64"MB\n", _countsWords >> 17);
  }

  _hashTable   = new uint64 [_hashTableWords];
  _buckets     = new uint64 [_bucketsWords];
  _countsWords = (flags & existDBcounts) ?             _countsWords  : 0;
  _counts      = (flags & existDBcounts) ? new uint64 [_countsWords] : 0L;

  //  These aren't strictly needed.  _buckets is cleared as it is initialied.  _hashTable
  //  is also cleared as it is initialized, but in the _compressedHash case, the last
  //  few words might be uninitialized.  They're unused.

  //memset(_hashTable, 0, sizeof(uint64) * _hashTableWords);
  //memset(_buckets,   0, sizeof(uint64) * _bucketsWords);  //  buckets is cleared as it is built
  //memset(_counts,    0, sizeof(uint64) * _countsWords);

  _hashTable[_hashTableWords-1] = 0;
  _hashTable[_hashTableWords-2] = 0;
  _hashTable[_hashTableWords-3] = 0;
  _hashTable[_hashTableWords-4] = 0;

  ////////////////////////////////////////////////////////////////////////////////
  //
  //  Make the hash table point to the start of the bucket, and reset
  //  the counting table -- we're going to use it to fill the buckets.
  //
  uint64  tmpPosition = 0;
  uint64  begPosition = 0;
  uint64  ptr         = 0;

  if (_compressedHash) {
    for (uint64 i=0; i<tableSizeInEntries; i++) {
      tmpPosition    = countingTable[i];
      countingTable[i] = begPosition;

      setDecodedValue(_hashTable, ptr, _hshWidth, begPosition);
      ptr         += _hshWidth;

      begPosition += tmpPosition;
    }

    setDecodedValue(_hashTable, ptr, _hshWidth, begPosition);
  } else {
    for (uint64 i=0; i<tableSizeInEntries; i++) {
      tmpPosition    = countingTable[i];
      countingTable[i] = begPosition;

      _hashTable[i] = begPosition;

      begPosition += tmpPosition;
    }

    //  Set the last position in the hash, but we don't care about
    //  the temporary counting table.
    //
    _hashTable[tableSizeInEntries] = begPosition;
  }


  ///////////////////////////////////////////////////////////////////////////////
  //
  //  3)  Build list of mers, placed into buckets
  //
  M = new merylStreamReader(prefix);
  C = new speedCounter("    %7.2f Mmers -- %5.2f Mmers/second\r", 1000000.0, 0x1fffff, beVerbose);

  while (M->nextMer()) {
    if ((lo <= M->theCount()) && (M->theCount() <= hi)) {
      if (_isForward)
        insertMer(HASH(M->theFMer()), CHECK(M->theFMer()), M->theCount(), countingTable);

      if (_isCanonical) {
        kMer  r = M->theFMer();
        r.reverseComplement();

        if (M->theFMer() < r)
          insertMer(HASH(M->theFMer()), CHECK(M->theFMer()), M->theCount(), countingTable);
        else
          insertMer(HASH(r), CHECK(r), M->theCount(), countingTable);
      }


      C->tick();
    }
  }

  delete C;
  delete M;
  delete [] countingTable;

  return(true);
}
