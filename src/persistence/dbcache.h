// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2020 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PERSIST_DB_CACHE_H
#define PERSIST_DB_CACHE_H

#include "dbconf.h"
#include "dbaccess.h"

#include <map>
#include <memory>

typedef void(UndoDataFunc)(const CDbOpLogs &pDbOpLogs);
typedef std::map<dbk::PrefixType, std::function<UndoDataFunc>> UndoDataFuncMap;

template<int32_t PREFIX_TYPE_VALUE, typename __KeyType, typename __ValueType>
class CCompositeKVCache {
public:
    static const dbk::PrefixType PREFIX_TYPE = (dbk::PrefixType)PREFIX_TYPE_VALUE;
public:
    typedef __KeyType   KeyType;
    typedef __ValueType ValueType;

    struct CacheValue {
        std::shared_ptr<ValueType> value = std::make_shared<ValueType>();
        bool is_modified = false;

        CacheValue() {}
        CacheValue(const ValueType &val, bool isModified)
            : value(std::make_shared<ValueType>(val)), is_modified(isModified) {}

        inline void Set(const CacheValue &other) {
            ASSERT(other.value);
            Set(*other.value, other.is_modified);
        }

        inline void Set(const ValueType &val, bool isModified) {
            ASSERT(value);
            *value = val;
            is_modified = isModified;
        }

        inline bool IsValueEmpty() const {
            return db_util::IsEmpty(*value);
        }

        inline void SetValueEmpty(bool isModified) {
            db_util::SetEmpty(*value);
            is_modified = isModified;
        }
    };

    typedef typename std::map<KeyType, CacheValue> Map;
    typedef typename std::map<KeyType, CacheValue>::iterator Iterator;
public:
    /**
     * Default constructor, must use set base to initialize before using.
     */
    CCompositeKVCache(): pBase(nullptr), pDbAccess(nullptr) {};

    CCompositeKVCache(CCompositeKVCache *pBaseIn): pBase(pBaseIn),
        pDbAccess(nullptr) {
        assert(pBaseIn != nullptr);
    };

    CCompositeKVCache(CDBAccess *pDbAccessIn): pBase(nullptr),
        pDbAccess(pDbAccessIn), is_calc_size(true) {
        assert(pDbAccessIn != nullptr);
        assert(pDbAccess->GetDbNameType() == GetDbNameEnumByPrefix(PREFIX_TYPE));
    };

    CCompositeKVCache(const CCompositeKVCache &other) {
        operator=(other);
    }

    CCompositeKVCache& operator=(const CCompositeKVCache& other) {
        pBase = other.pBase;
        pDbAccess = other.pDbAccess;
        // deep copy for map
        mapData.clear();
        for (auto otherItem : other.mapData) {
            mapData[otherItem.first].Set(otherItem.second);
        }
        pDbOpLogMap = other.pDbOpLogMap;
        is_calc_size = other.is_calc_size;
        size = other.size;

        return *this;
    }

    void SetBase(CCompositeKVCache *pBaseIn) {
        assert(pDbAccess == nullptr);
        assert(mapData.empty());
        pBase = pBaseIn;
    };

    void SetDbOpLogMap(CDBOpLogMap *pDbOpLogMapIn) {
        pDbOpLogMap = pDbOpLogMapIn;
    }

    bool IsCalcSize() const { return is_calc_size; }

    uint32_t GetCacheSize() const {
        return size;
    }

    bool GetData(const KeyType &key, ValueType &value) const {
        ASSERT(!db_util::IsEmpty(key));
        auto it = GetDataIt(key);
        if (!ValueIsEmpty(it)) {
            value = GetValueBy(it);
            return true;
        }
        return false;
    }

    bool GetData(const KeyType &key, const ValueType **value) const {
        ASSERT(value != nullptr && "the value pointer is NULL");
        ASSERT(!db_util::IsEmpty(key));
        auto it = GetDataIt(key);
        if (!ValueIsEmpty(it)) {
            *value = &(GetValueBy(it));
            return true;
        }
        return false;
    }

    bool SetData(const KeyType &key, const ValueType &value) {
        ASSERT(!db_util::IsEmpty(key));

        auto it = GetDataIt(key);
        if (it == mapData.end()) {
            AddOpLog(key, ValueType(), &value);

            AddDataToMap(key, value, true);
        } else {
            auto &valueRef = *it->second.value;
            AddOpLog(key, valueRef, &value);
            UpdateDataSize(valueRef, value);
            it->second.Set(value, true);
        }
        return true;
    }

    bool HasData(const KeyType &key) const {
        ASSERT(!db_util::IsEmpty(key));

        auto it = GetDataIt(key);
        return !ValueIsEmpty(it);
    }

    bool EraseData(const KeyType &key) {
        ASSERT(!db_util::IsEmpty(key));

        Iterator it = GetDataIt(key);
        if (!ValueIsEmpty(it)) {
            auto &valueRef = GetValueBy(it);
            DecDataSize(valueRef);
            AddOpLog(key, valueRef, nullptr);
            it->second.SetValueEmpty(true);
            IncDataSize(valueRef);
        }
        return true;
    }

    void Clear() {
        mapData.clear();
        size = 0;
    }

    void Flush() {
        assert(pBase != nullptr || pDbAccess != nullptr);
        if (pBase != nullptr) {
            assert(pDbAccess == nullptr);
            for (auto item : mapData) {
                if (item.second.is_modified) {
                    // TODO: move value to base for performance
                    pBase->SetDataToCache(item.first, *item.second.value);
                }
            }
        } else if (pDbAccess != nullptr) {
            assert(pBase == nullptr);
            CLevelDBBatch batch; // TODO: use only one batch for a block
            for (auto item : mapData) {
                if (item.second.is_modified) {
                    string key = dbk::GenDbKey(PREFIX_TYPE, item.first);
                    if (item.second.IsValueEmpty()) {
                        batch.Erase(key);
                    } else {
                        batch.Write(key, *item.second.value);
                    }
                }
            }
            pDbAccess->WriteBatch(batch);
        }

        Clear(); // TODO: use lru cache
    }

    void UndoData(const CDbOpLog &dbOpLog) {
        KeyType key;
        ValueType value;
        dbOpLog.Get(key, value);
        SetDataToCache(key, value);
    }

    void UndoDataList(const CDbOpLogs &dbOpLogs) {
        for (auto it = dbOpLogs.rbegin(); it != dbOpLogs.rend(); it++) {
            UndoData(*it);
        }
    }

    void RegisterUndoFunc(UndoDataFuncMap &undoDataFuncMap) {
        undoDataFuncMap[GetPrefixType()] = std::bind(&CCompositeKVCache::UndoDataList, this, std::placeholders::_1);
    }

    dbk::PrefixType GetPrefixType() const { return PREFIX_TYPE; }

    CDBAccess* GetDbAccessPtr() {
        CDBAccess* pRet = pDbAccess;
        if (pRet == nullptr && pBase != nullptr) {
            pRet = pBase->GetDbAccessPtr();
        }
        return pRet;
    }

    CCompositeKVCache<PREFIX_TYPE, KeyType, ValueType>* GetBasePtr() { return pBase; }

    Map& GetMapData() { return mapData; };
private:
    Iterator GetDataIt(const KeyType &key) const {
        Iterator it = mapData.find(key);
        if (it != mapData.end()) {
            return it;
        } else if (pBase != nullptr) {
            // find key-value at base cache
            auto baseIt = pBase->GetDataIt(key);
            if (baseIt != pBase->mapData.end()) {
                // add the found key-value to current mapData, igore the is_modified of Base
                return AddDataToMap(key, GetValueBy(baseIt), false);
            }
        } else if (pDbAccess != NULL) {
            // TODO: need to save the empty value to mapData for search performance?
            auto pDbValue = std::make_shared<ValueType>();
            CacheValue cacheValue;
            if (pDbAccess->GetData(PREFIX_TYPE, key, *cacheValue.value)) {
                return AddDataToMap(key, cacheValue);
            }
        }

        return mapData.end();
    }

    ValueType &GetValueBy(Iterator it) const {
        return *it->second.value;
    }

    inline bool ValueIsEmpty(Iterator it) const {
        return it == mapData.end() || it->second.IsValueEmpty();
    }

    // set data to cache only
    void SetDataToCache(const KeyType &key, const ValueType &value) {
        auto it = mapData.find(key);
        if (it != mapData.end()) {
            UpdateDataSize(GetValueBy(it), value);
            it->second.Set(value, true);
        } else {
            AddDataToMap(key, value, true);
        }
    }

    inline Iterator AddDataToMap(const KeyType &key, const ValueType &value, bool isModified) const {
        CacheValue cacheValue(value, isModified);
        return AddDataToMap(key, cacheValue);
    }

    inline Iterator AddDataToMap(const KeyType &key, CacheValue &cacheValue) const {

        ASSERT(!mapData.count(key));
        auto newRet = mapData.emplace(key, cacheValue);
        if (!newRet.second)
            throw runtime_error(strprintf("%s :  %s, alloc new cache item failed", __FUNCTION__, __LINE__));
        auto it = newRet.first;
        IncDataSize(key, GetValueBy(it));
        return it;
    }


    inline void IncDataSize(const KeyType &key, const ValueType &valueIn) const {
        if (is_calc_size) {
            size += CalcDataSize(key);
            size += CalcDataSize(valueIn);
        }
    }

    inline void IncDataSize(const ValueType &valueIn) const {
        if (is_calc_size)
            size += CalcDataSize(valueIn);
    }

    inline void DecDataSize(const ValueType &valueIn) const {
        if (is_calc_size) {
            uint32_t sz = CalcDataSize(valueIn);
            size = size > sz ? size - sz : 0;
        }
    }

    inline void UpdateDataSize(const ValueType &oldValue, const ValueType &newVvalue) const {
        if (is_calc_size) {
            size += CalcDataSize(newVvalue);
            uint32_t oldSz = CalcDataSize(oldValue);
            size = size > oldSz ? size - oldSz : 0;
        }
    }

    template <typename Data>
    inline uint32_t CalcDataSize(const Data &d) const {
        return ::GetSerializeSize(d, SER_DISK, CLIENT_VERSION);
    }

    inline void AddOpLog(const KeyType &key, const ValueType& oldValue, const ValueType *pNewValue) {
        if (pDbOpLogMap != nullptr) {
            CDbOpLog dbOpLog;
            #ifdef DB_OP_LOG_NEW_VALUE
                if (pNewValue != nullptr)
                    dbOpLog.Set(key, make_pair(oldValue, *pNewValue));
                else
                    dbOpLog.Set(key, make_pair(oldValue, ValueType()));
            #else
                dbOpLog.Set(key, oldValue);
            #endif
            pDbOpLogMap->AddOpLog(PREFIX_TYPE, dbOpLog);
        }

    }
private:
    mutable CCompositeKVCache<PREFIX_TYPE, KeyType, ValueType> *pBase = nullptr;
    CDBAccess *pDbAccess = nullptr;
    mutable Map mapData;
    CDBOpLogMap *pDbOpLogMap = nullptr;
    bool is_calc_size = false;
    mutable uint32_t size = 0;
};


template<int32_t PREFIX_TYPE_VALUE, typename __ValueType>
class CSimpleKVCache {
public:
    typedef __ValueType ValueType;
    static const dbk::PrefixType PREFIX_TYPE = (dbk::PrefixType)PREFIX_TYPE_VALUE;
public:
    /**
     * Default constructor, must use set base to initialize before using.
     */
    CSimpleKVCache(): pBase(nullptr), pDbAccess(nullptr) {};

    CSimpleKVCache(CSimpleKVCache *pBaseIn): pBase(pBaseIn),
        pDbAccess(nullptr) {
        assert(pBaseIn != nullptr);
    }

    CSimpleKVCache(CDBAccess *pDbAccessIn): pBase(nullptr),
        pDbAccess(pDbAccessIn) {
        assert(pDbAccessIn != nullptr);
    }

    CSimpleKVCache(const CSimpleKVCache &other) {
        operator=(other);
    }

    CSimpleKVCache& operator=(const CSimpleKVCache& other) {
        pBase = other.pBase;
        pDbAccess = other.pDbAccess;
        // deep copy for shared_ptr
        if (other.ptrData == nullptr) {
            ptrData = nullptr;
        } else {
            ptrData = make_shared<ValueType>(*other.ptrData);
        }
        pDbOpLogMap = other.pDbOpLogMap;
        return *this;
    }

    void SetBase(CSimpleKVCache *pBaseIn) {
        assert(pDbAccess == nullptr);
        assert(!ptrData && "Must SetBase before have any data");
        pBase = pBaseIn;
    }

    void SetDbOpLogMap(CDBOpLogMap *pDbOpLogMapIn) {
        pDbOpLogMap = pDbOpLogMapIn;
    }

    uint32_t GetCacheSize() const {
        if (!ptrData) {
            return 0;
        }

        return ::GetSerializeSize(*ptrData, SER_DISK, CLIENT_VERSION);
    }

    bool GetData(ValueType &value) const {
        auto ptr = GetDataPtr();
        if (ptr && !db_util::IsEmpty(*ptr)) {
            value = *ptr;
            return true;
        }
        return false;
    }

    bool GetData(const ValueType **value) const {
        assert(value != nullptr && "the value pointer is NULL");
        auto ptr = GetDataPtr();
        if (ptr && !db_util::IsEmpty(*ptr)) {
            *value = ptr.get();
            return true;
        }
        return false;
    }

    bool SetData(const ValueType &value) {
        if (!ptrData) {
            ptrData = db_util::MakeEmptyValue<ValueType>();
        }
        AddOpLog(*ptrData, &value);
        *ptrData = value;
        return true;
    }

    bool HasData() const {
        auto ptr = GetDataPtr();
        return ptr && !db_util::IsEmpty(*ptr);
    }

    bool EraseData() {
        auto ptr = GetDataPtr();
        if (ptr && !db_util::IsEmpty(*ptr)) {
            AddOpLog(*ptr, nullptr);
            db_util::SetEmpty(*ptr);
        }
        return true;
    }

    void Clear() {
        ptrData = nullptr;
    }

    void Flush() {
        assert(pBase != nullptr || pDbAccess != nullptr);
        if (ptrData) {
            if (pBase != nullptr) {
                assert(pDbAccess == nullptr);
                pBase->ptrData = ptrData;
            } else if (pDbAccess != nullptr) {
                assert(pBase == nullptr);
                pDbAccess->WriteBatch(PREFIX_TYPE, *ptrData);
            }
            ptrData = nullptr;
        }
    }

    void UndoData(const CDbOpLog &dbOpLog) {
        if (!ptrData) {
            ptrData = db_util::MakeEmptyValue<ValueType>();
        }
        dbOpLog.Get(*ptrData);
    }

    void UndoDataList(const CDbOpLogs &dbOpLogs) {
        for (auto it = dbOpLogs.rbegin(); it != dbOpLogs.rend(); it++) {
            UndoData(*it);
        }
    }

    void RegisterUndoFunc(UndoDataFuncMap &undoDataFuncMap) {
        undoDataFuncMap[GetPrefixType()] = std::bind(&CSimpleKVCache::UndoDataList, this, std::placeholders::_1);
    }

    dbk::PrefixType GetPrefixType() const { return PREFIX_TYPE; }

    std::shared_ptr<ValueType> GetDataPtr() const {

        if (ptrData) {
            return ptrData;
        } else if (pBase != nullptr){
            auto ptr = pBase->GetDataPtr();
            if (ptr) {
                ptrData = std::make_shared<ValueType>(*ptr);
                return ptrData;
            }
        } else if (pDbAccess != NULL) {
            auto ptrDbData = db_util::MakeEmptyValue<ValueType>();

            if (pDbAccess->GetData(PREFIX_TYPE, *ptrDbData)) {
                assert(!db_util::IsEmpty(*ptrDbData));
                ptrData = ptrDbData;
                return ptrData;
            }
        }
        return nullptr;
    }

private:
    inline void AddOpLog(const ValueType &oldValue, const ValueType *pNewValue) {
        if (pDbOpLogMap != nullptr) {
            CDbOpLog dbOpLog;
            #ifdef DB_OP_LOG_NEW_VALUE
                if (pNewValue != nullptr)
                    dbOpLog.Set(make_pair(oldValue, *pNewValue));
                else
                    dbOpLog.Set(make_pair(oldValue, ValueType()));
            #else
                dbOpLog.Set(oldValue);
            #endif
            pDbOpLogMap->AddOpLog(PREFIX_TYPE, dbOpLog);
        }

    }
private:
    mutable CSimpleKVCache<PREFIX_TYPE, ValueType> *pBase;
    CDBAccess *pDbAccess;
    mutable std::shared_ptr<ValueType> ptrData = nullptr;
    CDBOpLogMap *pDbOpLogMap                   = nullptr;
};

#endif  // PERSIST_DB_CACHE_H
