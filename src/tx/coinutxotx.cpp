// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "coinutxotx.h"
#include "main.h"
#include <string>
#include <cstdarg>

bool GetUtxoTxFromChain(TxID &txid, std::shared_ptr<CCoinUtxoTransferTx>& pPrevUtxoTx ) {
    if (!SysCfg().IsTxIndex())
        return false;

    std::shared_ptr<CBaseTx> pBaseTx;
    CDiskTxPos txPos;
    if (pCdMan->pBlockCache->ReadTxIndex(txid, txPos)) {
        LOCK(cs_main);
        CAutoFile file(OpenBlockFile(txPos, true), SER_DISK, CLIENT_VERSION);
        CBlockHeader header;

        try {
            file >> header;
            fseek(file, txPos.nTxOffset, SEEK_CUR);
            file >> pBaseTx;

            assert(pBaseTx);
            pPrevUtxoTx = dynamic_pointer_cast<CCoinUtxoTransferTx>(pBaseTx);
            if (!pPrevUtxoTx) {
                return ERRORMSG("The expected tx(%s) type is CCoinUtxoTransferTx, but read tx type is %s",
                                txid.ToString(), typeid(*pBaseTx).name());
            }

        } catch (std::exception &e) {
            throw runtime_error(strprintf("%s : Deserialize or I/O error - %s", __func__, e.what()).c_str());
        }
    } else {
        return ERRORMSG("utxo read preutxo tx index error");
    }
    return true;
}


bool ComputeRedeemScript(const uint8_t m, const uint8_t n, vector<string>& addresses, string &redeemScript) {
    for (const string addr : addresses)
        redeemScript += addr;

    redeemScript = strprintf("%c%u%s%u", '\xFF', m, redeemScript, n); //0xFF is the magic no to avoid conflict with PubKey Hash

    return true;
}

// internal function to this file only
bool ComputeRedeemScript(const CTxExecuteContext &context, const CMultiSignAddressCondIn &p2maIn, string &redeemScript) {
    CCacheWrapper &cw = *context.pCw;

    CAccount acct;
    vector<string> vAddress;
    for (const auto &uid : p2maIn.uids) {
        if (!cw.accountCache.GetAccount(uid, acct))
            return false;

        vAddress.push_back(acct.keyid.ToAddress());
    }

    ComputeRedeemScript(p2maIn.m, p2maIn.n, vAddress, redeemScript);

    return true;
}

bool ComputeMultiSignKeyId(const string &redeemScript, CKeyID &keyId) {
    uint160 redeemScriptHash = Hash160(redeemScript); //equal to RIPEMD160(SHAR256(redeemScript))
    keyId = CKeyID(redeemScriptHash);
    return true;
}


bool ComputeUtxoMultisignHash(const TxID &prevUtxoTxId, uint16_t prevUtxoTxVoutIndex,
                            const CAccount &txAcct, string &redeemScript, uint256 &hash) {
    CHashWriter ss(SER_GETHASH, CLIENT_VERSION);
    ss << prevUtxoTxId.ToString() << prevUtxoTxVoutIndex << txAcct.keyid.ToString() << redeemScript;
    hash = ss.GetHash();
    return true;
}

bool VerifyMultiSig(const CTxExecuteContext &context, const uint256 &utxoMultiSignHash, const CMultiSignAddressCondIn &p2maIn) {
    if (p2maIn.signatures.size() < p2maIn.m)
        return false;

    CCacheWrapper &cw = *context.pCw;

    string redeemScript("");
    if (!ComputeRedeemScript(context, p2maIn, redeemScript))
        return false;

    int verifyPassNum = 0;
    CAccount acct;
    for (const auto &signature : p2maIn.signatures) {
        for (const auto uid : p2maIn.uids) {
            if (!cw.accountCache.GetAccount(uid, acct) ||
                !acct.HaveOwnerPubKey())
                return false;

            if (VerifySignature(utxoMultiSignHash, signature, acct.owner_pubkey)) {
                verifyPassNum++;
                break;
            }
        }
    }
    bool verified = (verifyPassNum >= p2maIn.m);

    return verified;
}


inline bool CheckUtxoOutCondition( const CTxExecuteContext &context, const bool isPrevUtxoOut,
                                const CUserID &prevUtxoTxUid, const CAccount &txAcct,
                                const CUtxoInput &input, CUtxoCondStorageBean &cond, string &errMsg) {

    CCacheWrapper &cw = *context.pCw;

    switch (cond.sp_utxo_cond->cond_type) {
        case UtxoCondType::OP2SA : {
            CSingleAddressCondOut& theCond = dynamic_cast< CSingleAddressCondOut& > (*cond.sp_utxo_cond);

            if(isPrevUtxoOut) {
                CAccount outAcct;
                if (!cw.accountCache.GetAccount(theCond.uid, outAcct)) {
                    errMsg = strprintf("GetAccount failed: %s", theCond.uid.ToString());
                    return false;
                }

                if (outAcct.keyid != txAcct.keyid) {
                    errMsg = "keyid mismatch";
                    return false;
                }

            } else {
                if (theCond.uid.IsEmpty()) {
                    errMsg = "uid empty";
                    return false;
                }
            }
            break;
        }

        case UtxoCondType::OP2MA : {
            CMultiSignAddressCondOut& theCond = dynamic_cast< CMultiSignAddressCondOut& > (*cond.sp_utxo_cond);

            if (isPrevUtxoOut) { //previous UTXO output
                bool found = false;
                for (auto inputCond : input.conds) {
                    if (inputCond.sp_utxo_cond->cond_type == UtxoCondType::IP2MA) {
                        found = true;
                        CMultiSignAddressCondIn& p2maCondIn = dynamic_cast< CMultiSignAddressCondIn& > (*inputCond.sp_utxo_cond);
                        if (p2maCondIn.m > p2maCondIn.n) {
                            errMsg = strprintf("m (%d) > n (%d)", p2maCondIn.m, p2maCondIn.n);
                            return false;
                        }
                        if (p2maCondIn.m > 20 || p2maCondIn.n > 20) { //FIXME: replace 20 w/ sysparam
                            errMsg = strprintf("m (%d) > 20 or n(%d) > 20", p2maCondIn.m, p2maCondIn.n);
                            return false;
                        }
                        if (p2maCondIn.uids.size() != p2maCondIn.n) {
                            errMsg = strprintf("uids size=%d != n(%d)", p2maCondIn.uids.size(), p2maCondIn.n);
                            return false;
                        }
                        CKeyID multiSignKeyId;
                        string redeemScript("");
                        if (!ComputeRedeemScript(context, p2maCondIn, redeemScript) ||
                            !ComputeMultiSignKeyId(redeemScript, multiSignKeyId) ||
                            theCond.dest_multisign_keyid != multiSignKeyId) {
                            errMsg = "ComputeRedeemScript or ComputeMultiSignKeyId failed";
                            return false;
                        }

                        uint256 utxoMultiSignHash;
                        if (!ComputeUtxoMultisignHash(input.prev_utxo_txid, input.prev_utxo_vout_index, txAcct,
                                                    redeemScript, utxoMultiSignHash) ||
                            !VerifyMultiSig(context, utxoMultiSignHash, p2maCondIn)) {
                            errMsg = "ComputeUtxoMultisignHash or VerifyMultiSig failed";
                            return false;
                        }
                        break;
                    }
                }

                if (!found) {
                    errMsg = "cond not found";
                    return false;
                }
            } else { //current UTXO output
                if (theCond.dest_multisign_keyid.IsEmpty()) {
                    errMsg = "dest_multisign_keyid empty";
                    return false;
                }
            }
            break;
        }

        case UtxoCondType::OP2PH : {
            CPasswordHashLockCondOut& theCond = dynamic_cast< CPasswordHashLockCondOut& > (*cond.sp_utxo_cond);

            if (isPrevUtxoOut) {
                bool found = false;
                for (auto inputCond : input.conds) {
                    if (inputCond.sp_utxo_cond->cond_type == UtxoCondType::IP2PH) {
                        found = true;
                        CPasswordHashLockCondIn& p2phCondIn = dynamic_cast< CPasswordHashLockCondIn& > (*inputCond.sp_utxo_cond);

                        if (p2phCondIn.password.size() > 256) { //FIXME: sysparam
                            errMsg = strprintf("p2phCondIn.password.size() =%d > 256", p2phCondIn.password.size());
                            return false;
                        }

                        CKeyID prevUtxoTxKeyId ;
                        if(prevUtxoTxUid.is<CKeyID>())
                            prevUtxoTxKeyId = prevUtxoTxUid.get<CKeyID>();
                        else {
                            CAccount acct;
                            if (!cw.accountCache.GetAccount(prevUtxoTxUid, acct)) {
                                errMsg = strprintf("prevUtxoTxUid(%s)'s account not found", prevUtxoTxUid.ToString());
                                return false;
                            }
                            prevUtxoTxKeyId = acct.keyid;
                        }

                        if (theCond.password_proof_required) { //check if existing password ownership proof
                            string text = strprintf("%s%s%s%s%d", p2phCondIn.password,
                                                    prevUtxoTxKeyId.ToString(), txAcct.keyid.ToString(),
                                                    input.prev_utxo_txid.ToString(), input.prev_utxo_vout_index);

                            uint256 hash = Hash(text);
                            uint256 proof = uint256();
                            CRegIDKey regIdKey(txAcct.regid);
                            auto proofKey = std::make_tuple(input.prev_utxo_txid, CFixedUInt16(input.prev_utxo_vout_index), regIdKey);
                            if (context.pCw->txUtxoCache.GetUtxoPasswordProof(proofKey, proof)) {
                                errMsg = "GetUtxoPasswordProof failed";
                                return false;
                            }

                            if (hash != proof) {
                                errMsg = "hash != proof";
                                return false;
                            }

                        }
                        // further check if password_hash matches the hash of (TxUid,Password)

                        string text = strprintf("%s%s", prevUtxoTxKeyId.ToString(), p2phCondIn.password);
                        uint256 hash = Hash(text);
                        if (theCond.password_hash != hash) {
                            errMsg = "theCond.password_hash != hash";
                            return false;
                        }

                        break;
                    } else
                        continue;
                }
                if (!found) {
                    errMsg = "input cond not found";
                    return false;
                }

            } else { //output cond
                if (theCond.password_hash == uint256()) {
                    errMsg = "theCond.password_hash empty";
                    return false;
                }
            }
            break;
        }
        case UtxoCondType::OCLAIM_LOCK : {
            CClaimLockCondOut& theCond = dynamic_cast< CClaimLockCondOut& > (*cond.sp_utxo_cond);

            if (isPrevUtxoOut) {
                if ((uint64_t) context.height <= theCond.height) {
                    errMsg = strprintf("context.height(%llu) <= theCond.height(%llu)", (uint64_t) context.height, theCond.height);
                    return false;
                }

            } else { //output cond
                if (theCond.height == 0) {
                    errMsg = "theCond.height == 0";
                    return false;
                }
            }
            break;
        }
        case UtxoCondType::ORECLAIM_LOCK : {
            CReClaimLockCondOut& theCond = dynamic_cast< CReClaimLockCondOut& > (*cond.sp_utxo_cond);

            if (isPrevUtxoOut) {
                if (prevUtxoTxUid == txAcct.keyid) { // for reclaiming the coins
                    if (theCond.height == 0 || (uint64_t) context.height <= theCond.height) {
                        errMsg = "theCond.height == 0 or context.height <= theCond.height";
                        return false;
                    }
                }
            } else { //output cond
                if (theCond.height == 0) {
                    errMsg = "theCond.height == 0";
                    return false;
                }
            }
            break;
        }
        default: {
            string strInOut = isPrevUtxoOut ? "input" : "output";
            errMsg = strprintf("UtxoCondType unsupported: %", cond.sp_utxo_cond->cond_type);
            return false;
        }
    }
    return true;
}


bool CCoinUtxoTransferTx::CheckTx(CTxExecuteContext &context) {
    IMPLEMENT_DEFINE_CW_STATE;
    IMPLEMENT_CHECK_TX_MEMO;

    if ((txUid.is<CPubKey>()) && !txUid.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, public key is invalid"), REJECT_INVALID,
                        "bad-publickey");

    if (vins.size() > 100) //FIXME: need to use sysparam to replace 100
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, vins size > 100 error"), REJECT_INVALID,
                        "vins-size-too-large");

    if (vouts.size() > 100) //FIXME: need to use sysparam to replace 100
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, vouts size > 100 error"), REJECT_INVALID,
                        "vouts-size-too-large");

    if (vins.size() == 0 && vouts.size() == 0)
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, empty utxo error"), REJECT_INVALID,
                        "utxo-empty-err");

    uint64_t minFee;
    if (!GetTxMinFee(nTxType, context.height, fee_symbol, minFee)) { assert(false); }
    uint64_t minerMinFees = (2 * vins.size() + vouts.size()) * minFee;
    if (llFees < minerMinFees)
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, tx fee too small!"), REJECT_INVALID,
                        "bad-tx-fee-toosmall");

    CAccount srcAccount;
    if (!cw.accountCache.GetAccount(txUid, srcAccount)) //unregistered account not allowed to participate
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, read account failed"), REJECT_INVALID,
                        "bad-getaccount");

    uint64_t totalInAmount = 0;
    uint64_t totalOutAmount = 0;
    for (auto input : vins) {
        //load prevUtxoTx from blockchain
        std::shared_ptr<CCoinUtxoTransferTx> pPrevUtxoTx;
        if (!GetUtxoTxFromChain(input.prev_utxo_txid, pPrevUtxoTx))
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, failed to load prev utxo from chain!"), REJECT_INVALID,
                            "failed-to-load-prev-utxo-err");

        if ((uint16_t) pPrevUtxoTx->vouts.size() < input.prev_utxo_vout_index + 1)
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, prev utxo index OOR error!"), REJECT_INVALID,
                            "prev-utxo-index-OOR-err");

        //enumerate the prev tx out conditions to check if current input meets
        //the output conditions of the previous Tx
        for (auto cond : pPrevUtxoTx->vouts[input.prev_utxo_vout_index].conds) {
            string errMsg;
            if (!CheckUtxoOutCondition(context, true, pPrevUtxoTx->txUid, srcAccount, input, cond, errMsg))
                return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, CheckUtxoOutCondition error: %s!", errMsg),
                                REJECT_INVALID, "check-utox-cond-err");
        }

        totalInAmount += pPrevUtxoTx->vouts[input.prev_utxo_vout_index].coin_amount;
    }

    for (auto output : vouts) {
        if (output.coin_amount == 0)
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, zeror output amount error!"),
                            REJECT_INVALID, "zero-output-amount-err");

        //check each cond's validity
        for (auto cond : output.conds) {
            string errMsg;
            if (!CheckUtxoOutCondition(context, false, CUserID(), srcAccount, CUtxoInput(), cond, errMsg))
                return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, CheckUtxoOutCondition error: %s!", errMsg),
                                REJECT_INVALID, "check-utox-cond-err");
        }

        totalOutAmount += output.coin_amount;
    }

    uint64_t accountBalance;
    if ( !srcAccount.GetBalance(coin_symbol, BalanceType::FREE_VALUE, accountBalance) ||
         (accountBalance + totalInAmount < totalOutAmount + llFees) )
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, account balance coin_amount insufficient!\n"
                                    "accountBalance=%llu, totalInAmount=%llu, totalOutAmount=%llu, llFees=%llu\n"
                                    "srcAccount=%s coinSymbol=%s",
                                    accountBalance, totalInAmount, totalOutAmount, llFees,
                                    srcAccount.regid.ToString(), coin_symbol),
                        REJECT_INVALID, "insufficient-account-coin-amount");

    return true;
}

/**
 * only deal with account balance states change...nothing on UTXO
 */
bool CCoinUtxoTransferTx::ExecuteTx(CTxExecuteContext &context) {
    CCacheWrapper &cw       = *context.pCw;
    CValidationState &state = *context.pState;

    CAccount srcAccount;
    if (!cw.accountCache.GetAccount(txUid, srcAccount))
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::ExecuteTx, read txUid %s account info error",
                        txUid.ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");

    if (!GenerateRegID(context, srcAccount)) {
        return false;
    }

    uint64_t totalInAmount = 0;
    for (auto &input : vins) {
        auto utxoKey = std::make_pair(input.prev_utxo_txid, CFixedUInt16(input.prev_utxo_vout_index));
        if (!context.pCw->txUtxoCache.GetUtxoTx(utxoKey))
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, prev utxo already spent error!"), REJECT_INVALID,
                            "double-spend-prev-utxo-err");

        //load prevUtxoTx from blockchain
        std::shared_ptr<CCoinUtxoTransferTx> pPrevUtxoTx;
        if (!GetUtxoTxFromChain(input.prev_utxo_txid, pPrevUtxoTx))
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, failed to load prev utxo from chain!"), REJECT_INVALID,
                             "failed-to-load-prev-utxo-err");

        totalInAmount += pPrevUtxoTx->vouts[input.prev_utxo_vout_index].coin_amount;

        if (!context.pCw->txUtxoCache.DelUtoxTx(std::make_pair(input.prev_utxo_txid, CFixedUInt16(input.prev_utxo_vout_index))))
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, del prev utxo error!"), REJECT_INVALID,
                            "del-prev-utxo-err");

        uint256 proof = uint256();
        CRegIDKey regIdKey(srcAccount.regid);
        auto proofKey = std::make_tuple(input.prev_utxo_txid, CFixedUInt16(input.prev_utxo_vout_index), regIdKey);
        if (context.pCw->txUtxoCache.GetUtxoPasswordProof(proofKey, proof)) {
            context.pCw->txUtxoCache.DelUtoxPasswordProof(proofKey);
        }
    }

    uint64_t totalOutAmount = 0;
    uint16_t index = 0;
    for (auto &output : vouts) {
        totalOutAmount += output.coin_amount;
        auto utoxKey = std::make_pair(GetHash(), CFixedUInt16(index));
        if (!context.pCw->txUtxoCache.SetUtxoTx(utoxKey))
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, set utxo error!"), REJECT_INVALID, "set-utxo-err");

        index++;
    }

    uint64_t accountBalance;
    if ( !srcAccount.GetBalance(coin_symbol, BalanceType::FREE_VALUE, accountBalance) ||
         (accountBalance + totalInAmount < totalOutAmount + llFees) )
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, account balance coin_amount insufficient!"), REJECT_INVALID,
                        "insufficient-account-coin-amount");

    vector<CReceipt> receipts;

    uint64_t totalAccountOutAmount  = totalOutAmount + llFees;
    if (totalInAmount <  totalAccountOutAmount) {
        if (!srcAccount.OperateBalance(coin_symbol, SUB_FREE,  totalAccountOutAmount - totalInAmount)) {
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::ExecuteTx, failed to deduct coin_amount in txUid %s account",
                            txUid.ToString()), UPDATE_ACCOUNT_FAIL, "insufficient-fund-utxo");
        }
    } else if (totalInAmount > totalAccountOutAmount ) {
        if (!srcAccount.OperateBalance(coin_symbol, ADD_FREE, totalInAmount - totalAccountOutAmount)) {
            return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::ExecuteTx, failed to add coin_amount in txUid %s account",
                            txUid.ToString()), UPDATE_ACCOUNT_FAIL, "insufficient-fund-utxo");
        }
    }

    if (totalInAmount > totalOutAmount)
        receipts.emplace_back(CNullID(), txUid, coin_symbol, (totalInAmount - totalOutAmount), ReceiptCode::TRANSFER_UTXO_COINS);
    else if (totalInAmount < totalOutAmount)
        receipts.emplace_back(txUid, CNullID(), coin_symbol, (totalOutAmount - totalInAmount), ReceiptCode::TRANSFER_UTXO_COINS);

    if (!cw.accountCache.SaveAccount(srcAccount))
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::ExecuteTx, write source addr %s account info error",
                        txUid.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");

    if (!receipts.empty() && !cw.txReceiptCache.SetTxReceipts(GetHash(), receipts))
        return state.DoS(100, ERRORMSG("CCDPStakeTx::ExecuteTx, set tx receipts failed!! txid=%s",
                        GetHash().ToString()), REJECT_INVALID, "set-tx-receipt-failed");

    return true;
}


////////////////////////////////////////
/// class CCoinUtxoPasswordProofTx
////////////////////////////////////////
bool CCoinUtxoPasswordProofTx::CheckTx(CTxExecuteContext &context) {
    CValidationState &state = *context.pState;

    if ((txUid.is<CPubKey>()) && !txUid.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, public key is invalid"), REJECT_INVALID,
                        "bad-publickey");

    uint64_t minFee;
    if (!GetTxMinFee(nTxType, context.height, fee_symbol, minFee)) { assert(false); }
    if (llFees < minFee)
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, tx fee too small!"), REJECT_INVALID,
                        "bad-tx-fee-toosmall");

    if (utxo_txid.IsEmpty())
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, utxo txid empty error!"), REJECT_INVALID,
                        "uxto-txid-empty-err");

    if (password_proof.IsEmpty())
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::CheckTx, utxo password proof empty error!"), REJECT_INVALID,
                        "utxo-password-proof-empty-err");

    return true;
}

bool CCoinUtxoPasswordProofTx::ExecuteTx(CTxExecuteContext &context) {

    IMPLEMENT_DEFINE_CW_STATE

    CAccount srcAccount;
    if (!cw.accountCache.GetAccount(txUid, srcAccount))
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::ExecuteTx, read txUid %s account info error",
                        txUid.ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");

    if (!GenerateRegID(context, srcAccount))
        return false;

    if (!srcAccount.OperateBalance(fee_symbol, BalanceOpType::SUB_FREE, llFees))
        return state.DoS(100, ERRORMSG("CCoinUtxoPasswordProofTx::ExecuteTx, deduct fees from regId=%s failed,",
                                       txUid.ToString()), UPDATE_ACCOUNT_FAIL, "deduct-account-fee-failed");
    if (!cw.accountCache.SetAccount(txUid, srcAccount))
        return state.DoS(100, ERRORMSG("%s(), save tx account info failed! txuid=%s",
                                       __func__, txUid.ToString()), UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");

    CRegIDKey regIdKey(srcAccount.regid);
    if (!cw.txUtxoCache.SetUtxoPasswordProof(std::make_tuple(utxo_txid, CFixedUInt16(utxo_vout_index), regIdKey), password_proof))
        return state.DoS(100, ERRORMSG("CCoinUtxoTransferTx::ExecuteTx, bad saving utxo proof",
                        txUid.ToString()), READ_ACCOUNT_FAIL, "bad-save-utxo-passwordproof");

    return true;
}