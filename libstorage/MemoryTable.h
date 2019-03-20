/*
 * @CopyRight:
 * FISCO-BCOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FISCO-BCOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FISCO-BCOS.  If not, see <http://www.gnu.org/licenses/>
 * (c) 2016-2018 fisco-dev contributors.
 */
/** @file MemoryTable.h
 *  @author ancelmo
 *  @date 20180921
 */
#pragma once

#include "MemoryTable.h"
#include "Storage.h"
#include "Table.h"
#include <json/json.h>
#include <libdevcore/Guards.h>
#include <libdevcore/easylog.h>
#include <libdevcrypto/Hash.h>
#include <libprecompiled/Common.h>
#include <tbb/concurrent_unordered_map.h>
#include <boost/exception/diagnostic_information.hpp>
#include <boost/lexical_cast.hpp>
#include <type_traits>

namespace dev
{
namespace storage
{

class MemoryTable : public Table
{
public:
    using CacheType = std::map<std::string, Entries::Ptr>;
    using CacheItr = typename CacheType::iterator;
    using Ptr = std::shared_ptr<MemoryTable>;

    virtual ~MemoryTable(){};

    virtual Entries::Ptr select(const std::string& key, Condition::Ptr condition) override;

    virtual int update(const std::string& key, Entry::Ptr entry, Condition::Ptr condition,
            AccessOptions::Ptr options = std::make_shared<AccessOptions>()) override;

    virtual int insert(const std::string& key, Entry::Ptr entry,
            AccessOptions::Ptr options = std::make_shared<AccessOptions>(),
            bool needSelect = true) override;

    virtual int remove(const std::string& key, Condition::Ptr condition,
            AccessOptions::Ptr options = std::make_shared<AccessOptions>()) override;

    virtual h256 hash() override;

    virtual void clear() override { m_cache.clear(); }
    virtual bool empty() override { return m_cache.empty(); }

    void setStateStorage(Storage::Ptr amopDB) override { m_remoteDB = amopDB; }
    void setBlockHash(h256 blockHash) override { m_blockHash = blockHash; }
    void setBlockNum(int blockNum) override { m_blockNum = blockNum; }
    void setTableInfo(TableInfo::Ptr tableInfo) override { m_tableInfo = tableInfo; }

    bool checkAuthority(Address const& _origin) const override
    {
        if (m_tableInfo->authorizedAddress.empty())
            return true;
        auto it = find(m_tableInfo->authorizedAddress.cbegin(),
            m_tableInfo->authorizedAddress.cend(), _origin);
        return it != m_tableInfo->authorizedAddress.cend();
    }

    virtual TableData::Ptr dump() override
	{
    	auto data = std::make_shared<TableData>();

		data->info = m_tableInfo;
		data->entries = std::make_shared<Entries>();
		for (auto it : m_cache)
		{
			data->entries->addEntry(it.second);
		}

		for(size_t i=0; i<m_newEntries->size(); ++i) {
			data->entries->addEntry(m_newEntries->get(i));
		}

		return data;
    }

private:
    std::vector<size_t> processEntries(Entries::Ptr entries, Condition::Ptr condition)
    {
        std::vector<size_t> indexes;
        indexes.reserve(entries->size());
        if (condition->getConditions()->empty())
        {
            for (size_t i = 0; i < entries->size(); ++i)
                indexes.emplace_back(i);
            return indexes;
        }

        for (size_t i = 0; i < entries->size(); ++i)
        {
            Entry::Ptr entry = entries->get(i);
            if (processCondition(entry, condition))
            {
                indexes.push_back(i);
            }
        }

        return indexes;
    }

    bool processCondition(Entry::Ptr entry, Condition::Ptr condition)
    {
        try
        {
            for (auto& it : *condition->getConditions())
            {
                if (entry->getStatus() == Entry::Status::DELETED)
                {
                    return false;
                }

                std::string lhs = entry->getField(it.first);
                std::string rhs = it.second.second;

                if (it.second.first == Condition::Op::eq)
                {
                    if (lhs != rhs)
                    {
                        return false;
                    }
                }
                else if (it.second.first == Condition::Op::ne)
                {
                    if (lhs == rhs)
                    {
                        return false;
                    }
                }
                else
                {
                    if (lhs.empty())
                    {
                        lhs = "0";
                    }
                    if (rhs.empty())
                    {
                        rhs = "0";
                    }

                    int lhsNum = boost::lexical_cast<int>(lhs);
                    int rhsNum = boost::lexical_cast<int>(rhs);

                    switch (it.second.first)
                    {
                    case Condition::Op::eq:
                    case Condition::Op::ne:
                    {
                        break;
                    }
                    case Condition::Op::gt:
                    {
                        if (lhsNum <= rhsNum)
                        {
                            return false;
                        }
                        break;
                    }
                    case Condition::Op::ge:
                    {
                        if (lhsNum < rhsNum)
                        {
                            return false;
                        }
                        break;
                    }
                    case Condition::Op::lt:
                    {
                        if (lhsNum >= rhsNum)
                        {
                            return false;
                        }
                        break;
                    }
                    case Condition::Op::le:
                    {
                        if (lhsNum > rhsNum)
                        {
                            return false;
                        }
                        break;
                    }
                    }
                }
            }
        }
        catch (std::exception& e)
        {
            STORAGE_LOG(ERROR) << LOG_BADGE("MemoryTable") << LOG_DESC("Compare error")
                               << LOG_KV("msg", boost::diagnostic_information(e));
            return false;
        }

        return true;
    }

    bool isHashField(const std::string& _key)
    {
        if (!_key.empty())
        {
            return ((_key.substr(0, 1) != "_" && _key.substr(_key.size() - 1, 1) != "_") ||
                    (_key == STATUS));
        }
        // STORAGE_LOG(ERROR) << LOG_BADGE("MemoryTable") << LOG_DESC("Empty key error");
        return false;
    }

    void checkField(Entry::Ptr entry)
    {
        for (auto& it : *(entry->fields()))
        {
            if (m_tableInfo->fields.end() ==
                find(m_tableInfo->fields.begin(), m_tableInfo->fields.end(), it.first))
            {
                STORAGE_LOG(ERROR) << LOG_BADGE("MemoryTable") << LOG_DESC("field doen not exist")
                                  << LOG_KV("table name", m_tableInfo->name)
                                  << LOG_KV("field", it.first);
                throw std::invalid_argument("Invalid key.");
            }
        }
    }

    Storage::Ptr m_remoteDB;
    TableInfo::Ptr m_tableInfo;
    std::map<uint32_t, Entry::Ptr> m_cache;
    Entries::Ptr m_newEntries;
    h256 m_blockHash;
    int m_blockNum = 0;
};  // namespace storage

}  // namespace storage
}  // namespace dev
