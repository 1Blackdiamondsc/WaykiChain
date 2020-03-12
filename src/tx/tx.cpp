// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include <algorithm>

#include "tx.h"
#include "persistence/accountdb.h"
#include "persistence/contractdb.h"
#include "persistence/txdb.h"
#include "entities/account.h"
#include "commons/json/json_spirit_value.h"
#include "commons/json/json_spirit_writer_template.h"
#include "commons/json/json_spirit_utils.h"
#include "commons/serialize.h"
#include "crypto/hash.h"
#include "commons/util/util.h"
#include "main.h"
#include "vm/luavm/luavmrunenv.h"
#include "miner/miner.h"
#include "config/version.h"

using namespace json_spirit;
extern CCacheDBManager *pCdMan;


#define ERROR_TITLE(msg) (std::string(__FUNCTION__) + "(), " + msg)
#define BASE_TX_TITLE ERROR_TITLE(GetTxTypeName())

string GetTxType(const TxType txType) {
    auto it = kTxFeeTable.find(txType);
    if (it != kTxFeeTable.end())
        return std::get<0>(it->second);
    else
        return "";
}

bool GetTxMinFee(const TxType nTxType, int height, const TokenSymbol &symbol, uint64_t &feeOut) {
    if (pCdMan->pSysParamCache->GetMinerFee(nTxType, symbol, feeOut))
        return true ;

    const auto &iter = kTxFeeTable.find(nTxType);
    if (iter != kTxFeeTable.end()) {
        FeatureForkVersionEnum version = GetFeatureForkVersion(height);
        if (symbol == SYMB::WICC) {
            if (version >= MAJOR_VER_R2) {
                feeOut = std::get<2>(iter->second);
                return true;
            } else { //MAJOR_VER_R1  //Prior-stablecoin Release
                feeOut = std::get<1>(iter->second);
                return true;
            }
        } else if (symbol == SYMB::WUSD) {
            if (version >= MAJOR_VER_R2){
                feeOut = std::get<4>(iter->second);
                return true;
            } { //MAJOR_VER_R1 //Prior-stablecoin Release
                feeOut = std::get<3>(iter->second);
                return true;
            }
        }
    }
    return false;
}

bool CBaseTx::IsValidHeight(int32_t nCurrHeight, int32_t nTxCacheHeight) const {
    if (BLOCK_REWARD_TX == nTxType || UCOIN_BLOCK_REWARD_TX == nTxType || PRICE_MEDIAN_TX == nTxType)
        return true;

    if (valid_height > nCurrHeight + nTxCacheHeight / 2)
        return false;

    if (valid_height < nCurrHeight - nTxCacheHeight / 2)
        return false;

    return true;
}

bool CBaseTx::GenerateRegID(CTxExecuteContext &context, CAccount &account) {
    if (txUid.is<CPubKey>()) {
        account.owner_pubkey = txUid.get<CPubKey>();

        CRegID regId;
        if (context.pCw->accountCache.GetRegId(txUid, regId)) {
            // account has regid already, return
            return true;
        }

        // generate a new regid for the account
        account.regid = CRegID(context.height, context.index);
        if (!context.pCw->accountCache.SaveAccount(account))
            return context.pState->DoS(100, ERRORMSG("CBaseTx::GenerateRegID, save account info error"), WRITE_ACCOUNT_FAIL,
                             "bad-write-accountdb");
    }

    return true;
}

uint64_t CBaseTx::GetFuel(int32_t height, uint32_t fuelRate) {
    return (nRunStep == 0 || fuelRate == 0) ? 0 : std::ceil(nRunStep / 100.0f) * fuelRate;
}

bool CBaseTx::CheckBaseTx(CTxExecuteContext &context) {
    IMPLEMENT_DEFINE_CW_STATE;

    CAccount txAccount;
    bool foundAccount = cw.accountCache.GetAccount(txUid, txAccount);

    bool signatureValid = false;

    { //1. Tx signature check
        switch (nTxType) {
            case BLOCK_REWARD_TX:
            case PRICE_MEDIAN_TX:
            case UCOIN_REWARD_TX:
            case UCOIN_BLOCK_REWARD_TX: signatureValid = true; break;
            default: {
                if (GetFeatureForkVersion(context.height) < MAJOR_VER_R2) {
                    signatureValid = true; //due to a pre-existing bug and illegally issued unsigned vote Tx
                } else {
                    CPubKey pubKey;
                    if (txUid.is<CPubKey>()) {
                        pubKey = txUid.get<CPubKey>();
                    } else {
                        if (!foundAccount) {
                            return state.DoS(100, ERRORMSG("CheckBaseTx::CheckTx, read txUid %s account info error",
                                        txUid.ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");
                        }

                        if (txAccount.perms_sum == 0) {
                            return state.DoS(100, ERRORMSG("CheckBaseTx::CheckTx, perms_sum is zero error: txUid %s",
                                        txUid.ToString()), READ_ACCOUNT_FAIL, "bad-tx-sign");
                        }

                        pubKey = txAccount.owner_pubkey;
                    }

                    signatureValid = VerifySignature(context, pubKey);
                }
            }
        }
        if (!signatureValid)
            return state.DoS(100, ERRORMSG("CheckBaseTx::CheckTx, verify txUid %s sign failed", txUid.ToString()),
                            READ_ACCOUNT_FAIL, "bad-tx-sign");
    }

    { //2. check Tx fee
        switch (nTxType) {
            case BLOCK_REWARD_TX:
            case PRICE_MEDIAN_TX:
            case UCOIN_REWARD_TX:
            case UCOIN_BLOCK_REWARD_TX: break; //no fee required
            case LCONTRACT_DEPLOY_TX:
            case LCONTRACT_INVOKE_TX:
            case UCOIN_TRANSFER_TX: break;      //to be checked in Tx but not here
            default:
                if (!CheckFee(context)) return false;
        }
    }

    {
        switch (nTxType) {
            // case BLOCK_REWARD_TX:
            // case ACCOUNT_REGISTER_TX:
            case BCOIN_TRANSFER_TX:         IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_SEND_COIN) > 0);
            case LCONTRACT_DEPLOY_TX:       IMPLEMENT_CHECK_TX_REGID(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_DEPLOY_SC) > 0);
            case LCONTRACT_INVOKE_TX:       IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_INVOKE_SC) > 0);
            case DELEGATE_VOTE_TX:          IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_SEND_VOTE) > 0);
            case UCOIN_TRANSFER_MTX:        return ((txAccount.perms_sum & AccountPermType::PERM_SEND_COIN) > 0);
            case UCOIN_STAKE_TX:            return ((txAccount.perms_sum & AccountPermType::PERM_STAKE_COIN) > 0);
            case ASSET_ISSUE_TX:            IMPLEMENT_CHECK_TX_REGID(txUid);
            case UIA_UPDATE_TX:
            case UTXO_TRANSFER_TX:          IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
                                            IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_SEND_UTXO) > 0);
            case UTXO_PASSWORD_PROOF_TX:    IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
            case UCOIN_TRANSFER_TX:         IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
                                            IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_SEND_COIN) > 0);
            // case UCOIN_REWARD_TX:
            // case UCOIN_BLOCK_REWARD_TX:
            case UCONTRACT_DEPLOY_TX:       IMPLEMENT_CHECK_TX_REGID(txUid);
                                            IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
                                            return ((txAccount.perms_sum & AccountPermType::PERM_DEPLOY_SC) > 0);
            case UCONTRACT_INVOKE_TX:       IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
                                            IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
                                            return ((txAccount.perms_sum & AccountPermType::PERM_INVOKE_SC) > 0);
            case PRICE_FEED_TX:             IMPLEMENT_CHECK_TX_REGID(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_FEED_PRICE) > 0);
            // case PRICE_MEDIAN_TX:
            case CDP_STAKE_TX:              IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
                                            IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
            case CDP_REDEEM_TX:
            case CDP_LIQUIDATE_TX:          IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
                                            IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_CDP) > 0);
            // case NICKID_REGISTER_TX:
            case WASM_CONTRACT_TX:          return ((txAccount.perms_sum & AccountPermType::PERM_INVOKE_SC) > 0);
            // case DEX_TRADE_SETTLE_TX:
            // case DEX_CANCEL_ORDER_TX:
            case DEX_LIMIT_BUY_ORDER_TX:
            case DEX_LIMIT_SELL_ORDER_TX:
            case DEX_MARKET_BUY_ORDER_TX:
            case DEX_MARKET_SELL_ORDER_TX:
            case DEX_CANCEL_ORDER_TX:
            case DEX_ORDER_TX:
            case DEX_OPERATOR_ORDER_TX:
            case DEX_OPERATOR_UPDATE_TX:
            case DEX_OPERATOR_REGISTER_TX:  IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
            case DEX_TRADE_SETTLE_TX:       IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
                                            IMPLEMENT_CHECK_TX_REGID(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_DEX) > 0);
            case PROPOSAL_REQUEST_TX:       IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
            case PROPOSAL_APPROVAL_TX:      IMPLEMENT_CHECK_TX_REGID(txUid);
                                            return ((txAccount.perms_sum & AccountPermType::PERM_PROPOSE) > 0);
            case NICKID_REGISTER_TX:        IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
                                            IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
            default:
                return true;
        }
    }
}

bool CBaseTx::CheckTxFeeSufficient(const TokenSymbol &feeSymbol, const uint64_t llFees, const int32_t height) const {
    uint64_t minFee;
    if (!GetTxMinFee(nTxType, height, feeSymbol, minFee)) {
        assert(false && "Get tx min fee for WICC or WUSD");
        return false;
    }
    return llFees >= minFee;
}

// Transactions should check the signature size before verifying signature
bool CBaseTx::CheckSignatureSize(const vector<unsigned char> &signature) const {
    return signature.size() > 0 && signature.size() < MAX_SIGNATURE_SIZE;
}

Object CBaseTx::ToJson(const CAccountDBCache &accountCache) const {
    Object result;
    CKeyID srcKeyId;
    accountCache.GetKeyId(txUid, srcKeyId);
    result.push_back(Pair("txid",           GetHash().GetHex()));
    result.push_back(Pair("tx_type",        GetTxType(nTxType)));
    result.push_back(Pair("ver",            nVersion));
    result.push_back(Pair("tx_uid",         txUid.ToString()));
    result.push_back(Pair("from_addr",      srcKeyId.ToAddress()));
    result.push_back(Pair("fee_symbol",     fee_symbol));
    result.push_back(Pair("fees",           llFees));
    result.push_back(Pair("valid_height",   valid_height));
    result.push_back(Pair("signature",      HexStr(signature)));
    return result;
}

string CBaseTx::ToString(CAccountDBCache &accountCache) {
    return strprintf("txType=%s, hash=%s, ver=%d, pubkey=%s, llFees=%llu, keyid=%s, valid_height=%d",
                     GetTxType(nTxType), GetHash().ToString(), nVersion, txUid.get<CPubKey>().ToString(), llFees,
                     txUid.get<CPubKey>().GetKeyId().ToAddress(), valid_height);
}

bool CBaseTx::GetInvolvedKeyIds(CCacheWrapper &cw, set<CKeyID> &keyIds) {
    return AddInvolvedKeyIds({txUid}, cw, keyIds);
}

bool CBaseTx::AddInvolvedKeyIds(vector<CUserID> uids, CCacheWrapper &cw, set<CKeyID> &keyIds) {
    for (auto uid : uids) {
        CKeyID keyId;
        if (!cw.accountCache.GetKeyId(uid, keyId))
            return false;

        keyIds.insert(keyId);
    }
    return true;
}


bool CBaseTx::CheckFee(CTxExecuteContext &context, function<bool(CTxExecuteContext&, uint64_t)> minFeeChecker) const {
    // check fee value range
    if (!CheckBaseCoinRange(llFees))
        return context.pState->DoS(100, ERRORMSG("%s, tx fee out of range", __FUNCTION__), REJECT_INVALID,
                         "bad-tx-fee-toolarge");
    // check fee symbol valid
    if (!kFeeSymbolSet.count(fee_symbol))
        return context.pState->DoS(100,
                         ERRORMSG("%s, not support fee symbol=%s, only supports:%s", __FUNCTION__, fee_symbol,
                                  GetFeeSymbolSetStr()),
                         REJECT_INVALID, "bad-tx-fee-symbol");

    uint64_t minFee;
    if (!GetTxMinFee(nTxType, context.height, fee_symbol, minFee))
        return context.pState->DoS(100, ERRORMSG("GetTxMinFee failed, tx=%s", GetTxTypeName()),
            REJECT_INVALID, "get-tx-min-fee-failed");

    if (minFeeChecker != nullptr) {
        if (!minFeeChecker(context, minFee)) return false;
    } else {
        if (!CheckMinFee(context, minFee)) return false;
    }
    return true;
}

bool CBaseTx::CheckMinFee(CTxExecuteContext &context, uint64_t minFee) const {
    if (GetFeatureForkVersion(context.height) > MAJOR_VER_R3 && txUid.is<CPubKey>()) {
        minFee = 2 * minFee;
    }
    if (llFees < minFee){
        string err = strprintf("The given fee is too small: %llu < %llu sawi", llFees, minFee);
        return context.pState->DoS(100, ERRORMSG("%s, tx=%s, height=%d, fee_symbol=%s",
            err, GetTxTypeName(), context.height, fee_symbol), REJECT_INVALID, err);
    }
    return true;
}

bool CBaseTx::CheckTxAvailableFromVer(CTxExecuteContext &context, FeatureForkVersionEnum ver) {
    if (GetFeatureForkVersion(context.height) < ver)
        return context.pState->DoS(100, ERRORMSG("%s, tx type=%s is unavailable before height=%d",
                __func__, GetTxTypeName(), context.height),
                REJECT_INVALID, "unavailable-tx");
    return true;
}

bool CBaseTx::VerifySignature(CTxExecuteContext &context, const CPubKey &pubkey) {
    if (!CheckSignatureSize(signature)) {
        return context.pState->DoS(100, ERRORMSG("%s, tx signature size invalid", BASE_TX_TITLE), REJECT_INVALID,
                         "bad-tx-sig-size");
    }
    uint256 sighash = GetHash();
    if (!::VerifySignature(sighash, signature, pubkey)) {
        return context.pState->DoS(100, ERRORMSG("%s, tx signature error", BASE_TX_TITLE),
            REJECT_INVALID, "bad-tx-signature");
    }
    return true ;
}

/**################################ Universal Coin Transfer ########################################**/

string SingleTransfer::ToString(const CAccountDBCache &accountCache) const {
    return strprintf("to_uid=%s, coin_symbol=%s, coin_amount=%llu", to_uid.ToDebugString(), coin_symbol, coin_amount);
}

Object SingleTransfer::ToJson(const CAccountDBCache &accountCache) const {
    Object result;

    CKeyID desKeyId;
    accountCache.GetKeyId(to_uid, desKeyId);
    result.push_back(Pair("to_uid",      to_uid.ToString()));
    result.push_back(Pair("to_addr",     desKeyId.ToAddress()));
    result.push_back(Pair("coin_symbol", coin_symbol));
    result.push_back(Pair("coin_amount", coin_amount));

    return result;
}