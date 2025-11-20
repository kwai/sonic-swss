#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "tammgr.h"
#include "exec.h"
#include "shellcmd.h"

using namespace std;
using namespace swss;

TamMgr::TamMgr(DBConnector *cfgDb, DBConnector *appDb, const vector<string> &tableNames, const string &type) :
    Orch(cfgDb, tableNames),
    m_cfgTamSwitchTable(cfgDb, CFG_TAM_SWITCH_TABLE_NAME),
    m_cfgTamDMTable(cfgDb, CFG_TAM_DROPMONITOR_TABLE_NAME),
    m_cfgTamCollectorTable(cfgDb, CFG_TAM_COLLECTORS_TABLE_NAME),
    m_cfgTamSamplerTable(cfgDb, CFG_TAM_SAMPLINGRATE_TABLE_NAME),
    m_cfgTamFeaturesTable(cfgDb, CFG_TAM_FEATURES_TABLE_NAME),

    m_appTamDMTable(appDb, APP_TAM_DROPMONITOR_TABLE),
    m_appTamCollectorTable(appDb, APP_TAM_COLLECTOR_TABLE),
    m_appTamSamplerTable(appDb, APP_TAM_SAMPLER_TABLE),
    m_appTamINTable(appDb, APP_TAM_INT_TABLE),

    m_counter_db(std::shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0))),
    m_counterTamInitTable(std::unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_TAM_DM_TABLE))),

    m_gType(type)

{
    m_gDMEnable = false;
    m_gINTEnable = false;
}

// Start/stop tamagent service according to tam drop monitor feature value
void TamMgr::tamHandleService(bool enable)
{
    stringstream cmd;
    string res;

    SWSS_LOG_ENTER();

    if (enable)
    {
        cmd << "supervisorctl start tamagent";
    }
    else
    {
        cmd << "supervisorctl stop tamagent";
    }

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Command '%s' succeeded", cmd.str().c_str());
    }
}


void TamMgr::initTamCounterTable(bool enable)
{
    std::string init_key = "Init";
    if (enable)
    {
        vector<FieldValueTuple> initValues = {
            {"NULL", "NULL"},
        };

        m_counterTamInitTable->set(init_key, initValues);
    }
    else
    {
        // when mod switch set to disable, need to del all counter-db drop-monitor data
        vector<string> dm_counter_keys;
        m_counterTamInitTable->getKeys(dm_counter_keys);
        for (const auto &key: dm_counter_keys)
        {
            m_counterTamInitTable->del(key);
        }

        // del init_key to adapt KNP requirement
        m_counterTamInitTable->del(init_key);
    }

}

// Get memory drop monitor info from tam_global_info, and store info in the fvs
void TamMgr::tamGetDMInfo(vector<FieldValueTuple> &fvs, TamDMInfo &tam_global_info)
{
    FieldValueTuple fv1("switch-id", tam_global_info.switch_id);
    fvs.push_back(fv1);

    FieldValueTuple fv2("aging-interval", tam_global_info.aging_interval);
    fvs.push_back(fv2);

    FieldValueTuple fv3("poll-interval", tam_global_info.poll_interval);
    fvs.push_back(fv3);

    FieldValueTuple fv4("forward-mode", tam_global_info.forward_mode);
    fvs.push_back(fv4);

    FieldValueTuple fv5("monitor-mode", tam_global_info.monitor_mode);
    fvs.push_back(fv5);

    FieldValueTuple fv6("status", tam_global_info.status);
    fvs.push_back(fv6);
}


// Get memory collectpr info from collector, and store info in the fvs
void TamMgr::tamGetCollectorInfo(vector<FieldValueTuple> &fvs, Collector &collector)
{
    FieldValueTuple fv1("ip", collector.ip);
    fvs.push_back(fv1);

    FieldValueTuple fv2("port", collector.port);
    fvs.push_back(fv2);

    FieldValueTuple fv3("protocol", collector.protocol);
    fvs.push_back(fv3);
}


// Get memory sampler info from sampler, and store info in the fvs
void TamMgr::tamGetSamplerInfo(vector<FieldValueTuple> &fvs, Sampler &sampler)
{
    FieldValueTuple fv1("sampling-rate", sampler.sampling_rate);
    fvs.push_back(fv1);
}


// Store info in the memory structure TamDMInfo, and fill info into fvs to update appl_db
void TamMgr::tamCheckDMInfoAndFillValues(vector<FieldValueTuple> &values, vector<FieldValueTuple> &fvs)
{
    for (auto i : values)
    {
        SWSS_LOG_DEBUG("fvField is %s, fvValue is %s", fvField(i).c_str(), fvValue(i).c_str());

        if (fvField(i) == "switch-id")
        {
            m_gTamDMInfo.switch_id = fvValue(i);
        }
        if (fvField(i) == "aging-interval")
        {
            m_gTamDMInfo.aging_interval = fvValue(i);
        }
        if (fvField(i) == "poll-interval")
        {
            m_gTamDMInfo.poll_interval = fvValue(i);
        }
        if (fvField(i) == "forward-mode")
        {
            m_gTamDMInfo.forward_mode = fvValue(i);
        }
        if (fvField(i) == "monitor-mode")
        {
            m_gTamDMInfo.monitor_mode = fvValue(i);
        }
        if (fvField(i) == "status")
        {
            m_gTamDMInfo.status = fvValue(i);
        }

        FieldValueTuple fv(fvField(i), fvValue(i));
        fvs.push_back(fv);
    }
}


// Get memory int info from tam global info, and store info in the fvs
void TamMgr::tamGetINTInfoAndFillValues(vector<FieldValueTuple> &fvs, TamINTInfo &tam_global_info)
{
    FieldValueTuple fv1("device-id", tam_global_info.device_id);
    fvs.push_back(fv1);

    FieldValueTuple fv3("status", tam_global_info.status);
    fvs.push_back(fv3);
}


// Store info in the memory structure TamCollectorMap, and fill fvs to update appl_db
void TamMgr::tamCheckCollectorAndFillValues(string alias, vector<FieldValueTuple> &values, vector<FieldValueTuple> &fvs)
{
    for (auto i : values)
    {
        SWSS_LOG_DEBUG("fvField is %s, fvValue is %s", fvField(i).c_str(), fvValue(i).c_str());

        if (fvField(i) == "ip")
        {
            m_TamCollectorMap[alias].ip = fvValue(i);
        }
        if (fvField(i) == "port")
        {
            m_TamCollectorMap[alias].port = fvValue(i);
        }
        if (fvField(i) == "protocol")
        {
            m_TamCollectorMap[alias].protocol = fvValue(i);
        }

        FieldValueTuple fv(fvField(i), fvValue(i));
        fvs.push_back(fv);
    }
}

// Store info in the memory structure TamSamplerMap, and fill fvs to update appl_db
void TamMgr::tamCheckSamplerAndFillValues(string alias, vector<FieldValueTuple> &values, vector<FieldValueTuple> &fvs)
{
    for (auto i : values)
    {
        SWSS_LOG_DEBUG("fvField is %s, fvValue is %s", fvField(i).c_str(), fvValue(i).c_str());

        if (fvField(i) == "sampling-rate")
        {
            m_TamSamplerMap[alias].sampling_rate = fvValue(i);
            FieldValueTuple fv(fvField(i), fvValue(i));
            fvs.push_back(fv);
        }
    }
}


void TamMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        auto values = kfvFieldsValues(t);

        SWSS_LOG_DEBUG("Table name is %s", table.c_str());
        SWSS_LOG_DEBUG("Key is %s, op is %s", key.c_str(), op.c_str());

        for (auto i : values)
        {
            SWSS_LOG_DEBUG("field is %s, value is %s", fvField(i).c_str(), fvValue(i).c_str());
        }

        if (op == SET_COMMAND)
        {
            vector<FieldValueTuple> fvs;
            if (table == CFG_TAM_SWITCH_TABLE_NAME)
            {
                for (auto i : values)
                {
                    if (fvField(i) == "switch-id")
                    {
                        tamCheckDMInfoAndFillValues(values, fvs);
                        m_appTamDMTable.set(key, fvs);
                    }

                    if (fvField(i) == "device-id")
                    {
                        m_gTamINTInfo.device_id = fvValue(i);
                    }
                }
            }
            else if (table == CFG_TAM_DROPMONITOR_TABLE_NAME)
            {
                tamCheckDMInfoAndFillValues(values, fvs);
                m_appTamDMTable.set(key, fvs);
            }
            else if (table == CFG_TAM_COLLECTORS_TABLE_NAME)
            {
                tamCheckCollectorAndFillValues(key, values, fvs);
                m_appTamCollectorTable.set(key, fvs);
            }
            else if (table == CFG_TAM_SAMPLINGRATE_TABLE_NAME)
            {
                tamCheckSamplerAndFillValues(key, values, fvs);
                m_appTamSamplerTable.set(key, fvs);
            }
            else if (table == CFG_TAM_FEATURES_TABLE_NAME)
            {
                // global drop monitor switch
                if (key == "DROPMONITOR")
                {
                    for (auto i : values)
                    {
                        if (fvField(i) == "status")
                        {
                            bool dm_enable = false;
                            if (fvValue(i) == "ACTIVE")
                            {
                                dm_enable = true;
                            }
                            if (dm_enable == m_gDMEnable)
                            {
                                break;
                            }
                            m_gDMEnable = dm_enable;
                            tamHandleService(dm_enable);
                            initTamCounterTable(dm_enable);
                        }
                        tamCheckDMInfoAndFillValues(values, fvs);
                        m_appTamDMTable.set("global", fvs);
                    }
                }

                // process INT logic
                if (key == "INT")
                {
                    for (auto i : values)
                    {
                        if (fvField(i) == "status")
                        {
                            m_gTamINTInfo.status = fvValue(i);
                            if (fvValue(i) == "ACTIVE")
                            {
                                if (m_gINTEnable == true)
                                {
                                    break;
                                }
                                m_gINTEnable = true;
                            }
                            else if (fvValue(i) == "INACTIVE")
                            {
                                if (m_gINTEnable == false)
                                {
                                    break;
                                }
                                m_gINTEnable = false;
                            }
                            tamGetINTInfoAndFillValues(fvs, m_gTamINTInfo);
                            m_appTamINTable.set("global", fvs);
                        }
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            vector<FieldValueTuple> fvs;
            if (table == CFG_TAM_FEATURES_TABLE_NAME)
            {
                if (m_gDMEnable)
                {
                    tamHandleService(false);
                    initTamCounterTable(false);
                }
                m_gDMEnable = false;
                m_gTamDMInfo.status = "INACTIVE";

                tamGetDMInfo(fvs, m_gTamDMInfo);
                m_appTamDMTable.set("global", fvs);
            }
            else if (table == CFG_TAM_SWITCH_TABLE_NAME)
            {

                for (auto i : values)
                {
                    if (fvField(i) == "switch-id")
                    {
                        m_gTamDMInfo.switch_id = "";
                        tamGetDMInfo(fvs, m_gTamDMInfo);
                        m_appTamDMTable.set(key, fvs);
                    }

                    if (fvField(i) == "device-id")
                    {
                        m_gTamINTInfo.device_id = "";
                    }
                }
            }
            else if (table == CFG_TAM_DROPMONITOR_TABLE_NAME)
            {
                m_gTamDMInfo.aging_interval = "";
                m_gTamDMInfo.poll_interval = "";
                m_gTamDMInfo.forward_mode = "";
                m_gTamDMInfo.monitor_mode = "";

                tamGetDMInfo(fvs, m_gTamDMInfo);
                m_appTamDMTable.set(key, fvs);
            }
            else if (table == CFG_TAM_COLLECTORS_TABLE_NAME)
            {
                auto dmCollectorConf = m_TamCollectorMap.find(key);

                if (dmCollectorConf == m_TamCollectorMap.end())
                {
                    it++;
                    continue;
                }

                m_TamCollectorMap.erase(key);
                m_appTamCollectorTable.del(key);
            }
            else if (table == CFG_TAM_SAMPLINGRATE_TABLE_NAME)
            {
                auto dmSamplerConf = m_TamSamplerMap.find(key);

                if (dmSamplerConf == m_TamSamplerMap.end())
                {
                    it++;
                    continue;
                }

                m_TamSamplerMap.erase(key);
                m_appTamSamplerTable.del(key);
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}


