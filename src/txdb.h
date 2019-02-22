// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2018-2019 Crypticcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include "coins.h"
#include "dbwrapper.h"
#include "masternodes/masternodes.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

class CBlockFileInfo;
class CBlockIndex;
struct CDiskTxPos;
class uint256;
class CMasternode;
class CDismissVote;

namespace dpos{ class CRoundVote_p2p; }
namespace dpos{ class CTxVote_p2p; }


//! -dbcache default (MiB)
static const int64_t nDefaultDbCache = 450;
//! max. -dbcache (MiB)
static const int64_t nMaxDbCache = sizeof(void*) > 4 ? 16384 : 1024;
//! min. -dbcache in (MiB)
static const int64_t nMinDbCache = 4;

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB : public CCoinsView
{
protected:
    CDBWrapper db;
    CCoinsViewDB(std::string dbName, size_t nCacheSize, bool fMemory = false, bool fWipe = false);
public:
    CCoinsViewDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool GetSproutAnchorAt(const uint256 &rt, SproutMerkleTree &tree) const;
    bool GetSaplingAnchorAt(const uint256 &rt, SaplingMerkleTree &tree) const;
    bool GetNullifier(const uint256 &nf, ShieldedType type) const;
    bool GetCoins(const uint256 &txid, CCoins &coins) const;
    bool HaveCoins(const uint256 &txid) const;
    uint256 GetBestBlock() const;
    uint256 GetBestAnchor(ShieldedType type) const;
    bool BatchWrite(CCoinsMap &mapCoins,
                    const uint256 &hashBlock,
                    const uint256 &hashSproutAnchor,
                    const uint256 &hashSaplingAnchor,
                    CAnchorsSproutMap &mapSproutAnchors,
                    CAnchorsSaplingMap &mapSaplingAnchors,
                    CNullifiersMap &mapSproutNullifiers,
                    CNullifiersMap &mapSaplingNullifiers);
    bool GetStats(CCoinsStats &stats) const;
};

/** Access to the block database (blocks/index/) */
class CBlockTreeDB : public CDBWrapper
{
public:
    CBlockTreeDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
private:
    CBlockTreeDB(const CBlockTreeDB&);
    void operator=(const CBlockTreeDB&);
public:
    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool EraseBatchSync(const std::vector<const CBlockIndex*>& blockinfo);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo &fileinfo);
    bool ReadLastBlockFile(int &nFile);
    bool WriteReindexing(bool fReindex);
    bool ReadReindexing(bool &fReindex);
    bool ReadTxIndex(const uint256 &txid, CDiskTxPos &pos);
    bool WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> > &list);
    bool WriteFlag(const std::string &name, bool fValue);
    bool ReadFlag(const std::string &name, bool &fValue);
    bool LoadBlockIndexGuts();
};

/** Access to the masternodes database (masternodes/) */
class CMasternodesDB : public CDBWrapper
{
private:
//    CDBWrapper db;

public:
    CMasternodesDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

private:
    CMasternodesDB(CMasternodesDB const &);
    void operator=(CMasternodesDB const &);

public:
    void WriteMasternode(uint256 const & txid, CMasternode const & node, CDBBatch & batch);
    void EraseMasternode(uint256 const & txid, CDBBatch & batch);

    void WriteVote(uint256 const & txid, CDismissVote const & vote, CDBBatch & batch);
    void EraseVote(uint256 const & txid, CDBBatch & batch);

    void WriteUndo(uint256 const & txid, uint256 const & affectedNode, char undoType, CDBBatch & batch);
    void EraseUndo(uint256 const & txid, uint256 const & affectedItem, CDBBatch & batch);

    void ReadOperatorUndo(uint256 const & txid, CMasternodesView::COperatorUndoRec & value);
    void WriteOperatorUndo(uint256 const & txid, CMasternodesView::COperatorUndoRec const & value, CDBBatch & batch);
    void EraseOperatorUndo(uint256 const & txid, CDBBatch & batch);

    bool ReadTeam(int blockHeight, CTeam & team);
    bool WriteTeam(int blockHeight, CTeam const & team);
    bool EraseTeam(int blockHeight);

    bool LoadMasternodes(std::function<void(uint256 &, CMasternode &)> onNode);
    bool LoadVotes(std::function<void(uint256 &, CDismissVote &)> onVote);
    bool LoadUndo(std::function<void(uint256 &, uint256 &, char)> onUndo);
};


/** Access to the dPoS votes and blocks database (dpos/) */
class CDposDB : public CDBWrapper
{
public:
    CDposDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
    CDposDB(const CDposDB&) = delete;
    CDposDB& operator=(const CDposDB&) = delete;

public:
    void WriteViceBlock(const uint256& tip, const CBlock& block);
    void EraseViceBlock(const uint256& tip);

    void WriteRoundVote(const uint256& tip, const dpos::CRoundVote_p2p& vote);
    void EraseRoundVote(const uint256& tip);

    void WriteTxVote(const uint256& tip, const dpos::CTxVote_p2p& vote);
    void EraseTxVote(const uint256& tip);

    bool LoadViceBlocks(std::function<void(const uint256&, const CBlock&)> onViceBlock);
    bool LoadRoundVotes(std::function<void(const uint256&, const dpos::CRoundVote_p2p&)> onRoundVote);
    bool LoadTxVotes(std::function<void(const uint256&, const dpos::CTxVote_p2p&)> onTxVote);
};

#endif // BITCOIN_TXDB_H
