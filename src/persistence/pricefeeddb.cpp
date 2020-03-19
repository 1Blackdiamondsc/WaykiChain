// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pricefeeddb.h"

#include "config/scoin.h"
#include "main.h"
#include "tx/pricefeedtx.h"
#include "commons/types.h"

static inline bool ReadSlideWindow(CSysParamDBCache &sysParamCache, uint64_t &slideWindow, const char* pTitle) {
    if (!sysParamCache.GetParam(SysParamType::MEDIAN_PRICE_SLIDE_WINDOW_BLOCKCOUNT, slideWindow)) {
        return ERRORMSG("%s, read sys param MEDIAN_PRICE_SLIDE_WINDOW_BLOCKCOUNT error", pTitle);
    }
    return true;
}

void CConsecutiveBlockPrice::AddUserPrice(const int32_t blockHeight, const CRegID &regId, const uint64_t price) {
    mapBlockUserPrices[blockHeight][regId] = price;
}

void CConsecutiveBlockPrice::DeleteUserPrice(const int32_t blockHeight) {
    // Marked the value empty, the base cache will delete it when Flush() is called.
    mapBlockUserPrices[blockHeight].clear();
}

bool CConsecutiveBlockPrice::ExistBlockUserPrice(const int32_t blockHeight, const CRegID &regId) {
    if (mapBlockUserPrices.count(blockHeight) == 0)
        return false;

    return mapBlockUserPrices[blockHeight].count(regId);
}

////////////////////////////////////////////////////////////////////////////////
//CPricePointMemCache

bool CPricePointMemCache::ReleadBlocks(CSysParamDBCache &sysParamCache, CBlockIndex *pTipBlockIdx) {

    int64_t start       = GetTimeMillis();
    CBlockIndex *pBlockIdx  = pTipBlockIdx;

    uint64_t slideWindow = 0;
    if (!ReadSlideWindow(sysParamCache, slideWindow, __func__)) return false;
    uint32_t count       = 0;

    while (pBlockIdx && count < slideWindow ) {
        CBlock block;
        if (!ReadBlockFromDisk(pBlockIdx, block))
            return ERRORMSG("%s() : read block=[%d]%s failed", __func__, pBlockIdx->height,
                    pBlockIdx->GetBlockHash().ToString());

        if (!AddPriceByBlock(block))
            return ERRORMSG("%d(), add block=[%d]%s to price point memory cache failed",
                    __func__, pBlockIdx->height, pBlockIdx->GetBlockHash().ToString());

        pBlockIdx = pBlockIdx->pprev;
        ++count;
    }
    LogPrint(BCLog::INFO, "Reload the latest %d blocks to price point memory cache (%d ms)\n", count, GetTimeMillis() - start);
    return true;
}

bool CPricePointMemCache::PushBlock(CSysParamDBCache &sysParamCache, CBlockIndex *pTipBlockIdx) {

    uint64_t slideWindow = 0;
    if (!ReadSlideWindow(sysParamCache, slideWindow, __func__)) return false;
    // remove the oldest block
    if (pTipBlockIdx->height > (int32_t)slideWindow) {
        CBlockIndex *pDeleteBlockIndex = pTipBlockIdx;
        int32_t height           = slideWindow;
        while (pDeleteBlockIndex && height-- > 0) {
            pDeleteBlockIndex = pDeleteBlockIndex->pprev;
        }

        CBlock deleteBlock;
        if (!ReadBlockFromDisk(pDeleteBlockIndex, deleteBlock)) {
            return ERRORMSG("%s() : read block=[%d]%s failed", __func__, pDeleteBlockIndex->height,
                    pDeleteBlockIndex->GetBlockHash().ToString());
        }

        if (!DeleteBlockFromCache(deleteBlock)) {
            return ERRORMSG("%s() : delete block==[%d]%s from price point memory cache failed",
                    __func__, pDeleteBlockIndex->height, pDeleteBlockIndex->GetBlockHash().ToString());
        }
    }
    return true;
}

bool CPricePointMemCache::UndoBlock(CSysParamDBCache &sysParamCache, CBlockIndex *pTipBlockIdx) {

    uint64_t slideWindow = 0;
    if (!ReadSlideWindow(sysParamCache, slideWindow, __func__)) return false;
    // Delete the disconnected block's pricefeed items from price point memory cache.
    if (!DeleteBlockPricePoint(pTipBlockIdx->height)) {
        return ERRORMSG("%s() : delete block=[%d]%s from price point memory cache failed",
                __func__, pTipBlockIdx->height, pTipBlockIdx->GetBlockHash().ToString());
    }
    if (pTipBlockIdx->height > (int32_t)slideWindow) {
        CBlockIndex *pReLoadBlockIndex = pTipBlockIdx;
        int32_t nCacheHeight           = slideWindow;
        while (pReLoadBlockIndex && nCacheHeight-- > 0) {
            pReLoadBlockIndex = pReLoadBlockIndex->pprev;
        }

        CBlock reLoadblock;
        if (!ReadBlockFromDisk(pReLoadBlockIndex, reLoadblock)) {
            return ERRORMSG("%s() : read block=[%d]%s failed", __func__, pReLoadBlockIndex->height,
                    pReLoadBlockIndex->GetBlockHash().ToString());
        }

        if (!AddPriceByBlock(reLoadblock)) {
            return ERRORMSG("%s() : add block=[%d]%s into price point memory cache failed",
                    __func__, pReLoadBlockIndex->height, pReLoadBlockIndex->GetBlockHash().ToString());
        }
    }
    return true;
}

bool CPricePointMemCache::AddPrice(const int32_t blockHeight, const CRegID &regId,
                                                    const vector<CPricePoint> &pps) {
    for (CPricePoint pp : pps) {
        if (ExistBlockUserPrice(blockHeight, regId, pp.GetCoinPricePair())) {
            LogPrint(BCLog::PRICEFEED,
                     "CPricePointMemCache::AddPrice, existed block user price, "
                     "height: %d, redId: %s, pricePoint: %s\n",
                     blockHeight, regId.ToString(), pp.ToString());
            return false;
        }

        CConsecutiveBlockPrice &cbp = mapCoinPricePointCache[pp.GetCoinPricePair()];
        cbp.AddUserPrice(blockHeight, regId, pp.GetPrice());
        LogPrint(BCLog::PRICEFEED,
                 "CPricePointMemCache::AddPrice, add block user price, "
                 "height: %d, redId: %s, pricePoint: %s\n",
                 blockHeight, regId.ToString(), pp.ToString());
    }

    return true;
}

bool CPricePointMemCache::ExistBlockUserPrice(const int32_t blockHeight, const CRegID &regId,
                                              const PriceCoinPair &coinPricePair) {
    if (mapCoinPricePointCache.count(coinPricePair) &&
        mapCoinPricePointCache[coinPricePair].ExistBlockUserPrice(blockHeight, regId))
        return true;

    if (pBase)
        return pBase->ExistBlockUserPrice(blockHeight, regId, coinPricePair);
    else
        return false;
}

bool CPricePointMemCache::AddPriceByBlock(const CBlock &block) {
    // index[0]: block reward transaction
    // index[1 ~ n - 1]: price feed transactions if existing
    // index[n]: block median price transaction

    if (block.vptx.size() < 3) {
        return true;
    }

    // More than 3 transactions in the block.
    for (uint32_t i = 1; i < block.vptx.size(); ++i) {
        if (!block.vptx[i]->IsPriceFeedTx()) {
            break;
        }

        CPriceFeedTx *priceFeedTx = (CPriceFeedTx *)block.vptx[i].get();
        AddPrice(block.GetHeight(), priceFeedTx->txUid.get<CRegID>(), priceFeedTx->price_points);
    }

    return true;
}

bool CPricePointMemCache::DeleteBlockPricePoint(const int32_t blockHeight) {
    if (mapCoinPricePointCache.empty()) {
        // TODO: multi stable coin
        mapCoinPricePointCache[PriceCoinPair(SYMB::WICC, SYMB::USD)].DeleteUserPrice(blockHeight);
        mapCoinPricePointCache[PriceCoinPair(SYMB::WGRT, SYMB::USD)].DeleteUserPrice(blockHeight);
    } else {
        for (auto &item : mapCoinPricePointCache) {
            item.second.DeleteUserPrice(blockHeight);
        }
    }

    return true;
}

bool CPricePointMemCache::DeleteBlockFromCache(const CBlock &block) { return DeleteBlockPricePoint(block.GetHeight()); }

void CPricePointMemCache::BatchWrite(const CoinPricePointMap &mapCoinPricePointCacheIn) {
    for (const auto &item : mapCoinPricePointCacheIn) {
        // map<int32_t /* block height */, map<CRegID, uint64_t /* price */>>
        const auto &mapBlockUserPrices = item.second.mapBlockUserPrices;
        for (const auto &userPrice : mapBlockUserPrices) {
            if (userPrice.second.empty()) {
                mapCoinPricePointCache[item.first /* PriceCoinPair */].mapBlockUserPrices.erase(userPrice.first /* height */);
            } else {
                // map<CRegID, uint64_t /* price */>;
                for (const auto &priceItem : userPrice.second) {
                    mapCoinPricePointCache[item.first /* PriceCoinPair */]
                        .mapBlockUserPrices[userPrice.first /* height */]
                        .emplace(priceItem.first /* CRegID */, priceItem.second /* price */);
                }
            }
        }
    }
}

void CPricePointMemCache::SetBaseViewPtr(CPricePointMemCache *pBaseIn) {
    pBase                        = pBaseIn;
}

void CPricePointMemCache::Flush() {
    assert(pBase);

    pBase->BatchWrite(mapCoinPricePointCache);
    mapCoinPricePointCache.clear();
}

bool CPricePointMemCache::GetBlockUserPrices(const PriceCoinPair &coinPricePair, set<int32_t> &expired,
                                             BlockUserPriceMap &blockUserPrices) {
    const auto &iter = mapCoinPricePointCache.find(coinPricePair);
    if (iter != mapCoinPricePointCache.end()) {
        const auto &mapBlockUserPrices = iter->second.mapBlockUserPrices;
        for (const auto &item : mapBlockUserPrices) {
            if (item.second.empty()) {
                expired.insert(item.first);
            } else if (expired.count(item.first) || blockUserPrices.count(item.first)) {
                // TODO: log
                continue;
            } else {
                // Got a valid item.
                blockUserPrices[item.first] = item.second;
            }
        }
    }

    if (pBase != nullptr) {
        return pBase->GetBlockUserPrices(coinPricePair, expired, blockUserPrices);
    }

    return true;
}

bool CPricePointMemCache::GetBlockUserPrices(const PriceCoinPair &coinPricePair, BlockUserPriceMap &blockUserPrices) {
    set<int32_t /* block height */> expired;
    if (!GetBlockUserPrices(coinPricePair, expired, blockUserPrices)) {
        // TODO: log
        return false;
    }

    return true;
}

CMedianPriceDetail CPricePointMemCache::ComputeBlockMedianPrice(const int32_t blockHeight, const uint64_t slideWindow,
                                                      const PriceCoinPair &coinPricePair) {
    // 1. merge block user prices with base cache.
    BlockUserPriceMap blockUserPrices;
    if (!GetBlockUserPrices(coinPricePair, blockUserPrices) || blockUserPrices.empty()) {
        return CMedianPriceDetail(); // {0, 0}
    }

    // 2. compute block median price.
    return ComputeBlockMedianPrice(blockHeight, slideWindow, blockUserPrices);
}

CMedianPriceDetail CPricePointMemCache::ComputeBlockMedianPrice(const int32_t blockHeight, const uint64_t slideWindow,
                                                      const BlockUserPriceMap &blockUserPrices) {
    // for (const auto &item : blockUserPrices) {
    //     string price;
    //     for (const auto &userPrice : item.second) {
    //         price += strprintf("{user:%s, price:%lld}", userPrice.first.ToString(), userPrice.second);
    //     }

    //     LogPrint(BCLog::PRICEFEED, "CPricePointMemCache::ComputeBlockMedianPrice, height: %d, userPrice: %s\n",
    //              item.first, price);
    // }

    CMedianPriceDetail priceDetail;
    vector<uint64_t> prices;
    int32_t beginBlockHeight = std::max<int32_t>((blockHeight - slideWindow), 0);
    for (int32_t height = blockHeight; height > beginBlockHeight; --height) {
        const auto &iter = blockUserPrices.find(height);
        if (iter != blockUserPrices.end()) {
            if (height == blockHeight)
                priceDetail.last_feed_height = blockHeight; // current block has price feed

            for (const auto &userPrice : iter->second) {
                prices.push_back(userPrice.second);
            }
        }
    }

    // for (const auto &item : prices) {
    //     LogPrint(BCLog::PRICEFEED, "CPricePointMemCache::ComputeBlockMedianPrice, found a candidate price: %llu\n", item);
    // }

    priceDetail.price = ComputeMedianNumber(prices);
    LogPrint(BCLog::PRICEFEED,
             "CPricePointMemCache::ComputeBlockMedianPrice, blockHeight: %d, computed median number: %llu\n",
             blockHeight, priceDetail.price);

    return priceDetail;
}

uint64_t CPricePointMemCache::ComputeMedianNumber(vector<uint64_t> &numbers) {
    int32_t size = numbers.size();
    if (size < 2) {
        return size == 0 ? 0 : numbers[0];
    }
    sort(numbers.begin(), numbers.end());
    return (size % 2 == 0) ? (numbers[size / 2 - 1] + numbers[size / 2]) / 2 : numbers[size / 2];
}

CMedianPriceDetail CPricePointMemCache::GetMedianPrice(const int32_t blockHeight, const uint64_t slideWindow,
                                             const PriceCoinPair &coinPricePair) {
    CMedianPriceDetail priceDetail;
    priceDetail = ComputeBlockMedianPrice(blockHeight, slideWindow, coinPricePair);

    if (priceDetail.price == 0) {
        auto it = latest_median_prices.find(coinPricePair);
        priceDetail = it != latest_median_prices.end() ? it->second : CMedianPriceDetail();

        LogPrint(BCLog::PRICEFEED,
                 "CPricePointMemCache::GetMedianPrice, use previous block median price: blockHeight: %d, "
                 "coin_pair=%s:%s price_detail={%s}\n",
                 blockHeight, coinPricePair.first, coinPricePair.second, priceDetail.ToString());
    }

    return priceDetail;
}

bool CPricePointMemCache::CalcMedianPrices(CCacheWrapper &cw, const int32_t blockHeight, PriceMap &medianPrices) {

    // TODO: support more price pair
    uint64_t slideWindow = 0;
    if (!ReadSlideWindow(cw.sysParamCache, slideWindow, __func__)) return false;

    latest_median_prices = cw.priceFeedCache.GetMedianPrices();

    PriceCoinPair bcoinPricePair(SYMB::WICC, SYMB::USD);
    CMedianPriceDetail bcoinMedianPrice = GetMedianPrice(blockHeight, slideWindow, bcoinPricePair);
    medianPrices.emplace(bcoinPricePair, bcoinMedianPrice.price);
    LogPrint(BCLog::PRICEFEED, "CPricePointMemCache::CalcBlockMedianPrices, blockHeight: %d, price: %s/%s -> %llu\n",
             blockHeight, SYMB::WICC, SYMB::USD, bcoinMedianPrice.price);

    PriceCoinPair fcoinPricePair(SYMB::WGRT, SYMB::USD);
    CMedianPriceDetail fcoinMedianPrice = GetMedianPrice(blockHeight, slideWindow, fcoinPricePair);
    medianPrices.emplace(fcoinPricePair, fcoinMedianPrice.price);
    LogPrint(BCLog::PRICEFEED, "CPricePointMemCache::CalcBlockMedianPrices, blockHeight: %d, price: %s/%s -> %llu\n",
             blockHeight, SYMB::WGRT, SYMB::USD, fcoinMedianPrice.price);

    return true;
}

bool CPricePointMemCache::CalcMedianPriceDetails(CCacheWrapper &cw, const int32_t blockHeight, PriceDetailMap &medianPrices)  {

    // TODO: support more price pair
    uint64_t slideWindow = 0;
    if (!ReadSlideWindow(cw.sysParamCache, slideWindow, __func__)) return false;

    latest_median_prices = cw.priceFeedCache.GetMedianPrices();

    set<PriceCoinPair> coinPairSet;
    if (cw.priceFeedCache.GetFeedCoinPairs(coinPairSet)) {
        // check the base asset has price feed permission
        for (auto it = coinPairSet.begin(); it != coinPairSet.end(); ) {
            CAsset asset;
            if (!cw.assetCache.GetAsset(it->first, asset)) {
                return ERRORMSG("%s(), the asset of base_symbol=%s not exist", __func__, it->first);
            }
            if (!asset.HasPerms(AssetPermType::PERM_PRICE_FEED)) {
                LogPrint(BCLog::PRICEFEED, "%s(), the asset of base_symbol=%s not have PERM_PRICE_FEED",
                        __func__, it->first);
                it = coinPairSet.erase(it);
                continue;
            }
            it++;
        }
    }
    // get hard code PriceFeedCoinPairs
    coinPairSet.insert(kPriceFeedCoinPairSet.begin(), kPriceFeedCoinPairSet.end());

    for (const auto& item : coinPairSet) {
        CMedianPriceDetail bcoinMedianPrice = GetMedianPrice(blockHeight, slideWindow, item);
        if (bcoinMedianPrice.price == 0) {
            LogPrint(BCLog::PRICEFEED, "%s(), calc median price=0 of coin_pair={%s}, ignore, height=%d\n",
                    __func__, CoinPairToString(item), blockHeight);
            continue;
        }
        medianPrices.emplace(item, bcoinMedianPrice);
        LogPrint(BCLog::PRICEFEED, "%s(), calc median price=%llu of coin_pair={%s}, height=%d\n",
                blockHeight, bcoinMedianPrice.price, CoinPairToString(item), blockHeight);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// CPriceFeedCache

uint64_t CPriceFeedCache::GetMedianPrice(const PriceCoinPair &coinPricePair) const {
    PriceDetailMap medianPrices;
    if (median_price_cache.GetData(medianPrices)) {
        auto it = medianPrices.find(coinPricePair);
        if (it != medianPrices.end())
            return it->second.price;
    }
    return 0;
}

PriceDetailMap CPriceFeedCache::GetMedianPrices() const {
    PriceDetailMap medianPrices;
    if (median_price_cache.GetData(medianPrices)) {
        return medianPrices;
    }
    return {};
}

bool CPriceFeedCache::SetMedianPrices(const PriceDetailMap &medianPrices) {
    return median_price_cache.SetData(medianPrices);
}

bool CPriceFeedCache::AddFeedCoinPair(TokenSymbol baseSymbol, TokenSymbol quoteSymbol) {
    if((baseSymbol == SYMB::WICC || baseSymbol == SYMB::WGRT) && quoteSymbol == SYMB::USD)
        return true ;

    set<PriceCoinPair> coinPairs;
    price_feed_coin_pairs_cache.GetData(coinPairs);
    if(coinPairs.count(PriceCoinPair(baseSymbol, quoteSymbol)) > 0 )
        return true;

    coinPairs.insert(PriceCoinPair(baseSymbol, quoteSymbol));
    return price_feed_coin_pairs_cache.SetData(coinPairs);
}

bool CPriceFeedCache::EraseFeedCoinPair(TokenSymbol baseSymbol, TokenSymbol quoteSymbol) {

    if((baseSymbol == SYMB::WICC || baseSymbol == SYMB::WGRT) && quoteSymbol == SYMB::USD)
        return true ;

    PriceCoinPair coinPair(baseSymbol, quoteSymbol);
    set<PriceCoinPair> coins ;
    price_feed_coin_pairs_cache.GetData(coins);
    if(coins.count(coinPair) == 0 )
        return true ;

    coins.erase(coinPair) ;
    return price_feed_coin_pairs_cache.SetData(coins);
}

bool CPriceFeedCache::HasFeedCoinPair(TokenSymbol baseSymbol,TokenSymbol quoteSymbol) {
    // WICC:USD is the default staked coin pair of cdp
    // WGRT:USD is need by forced-liquidate cdp for inflating WGRT
    if((baseSymbol == SYMB::WICC || baseSymbol == SYMB::WGRT) && quoteSymbol == SYMB::USD)
        return true ;

    set<PriceCoinPair> coins ;
    price_feed_coin_pairs_cache.GetData(coins);
    return (coins.count(make_pair(baseSymbol,quoteSymbol)) != 0 ) ;
}

bool CPriceFeedCache::GetFeedCoinPairs(set<PriceCoinPair>& coinPairSet) {
    price_feed_coin_pairs_cache.GetData(coinPairSet);
    return true ;
}
