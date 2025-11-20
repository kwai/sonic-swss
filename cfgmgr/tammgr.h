#pragma once

#include "dbconnector.h"
#include "orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>
#include <algorithm>

namespace swss {

/* Drop monitor global configuration*/
struct TamDMInfo
{
    std::string switch_id;
    std::string aging_interval;
    std::string poll_interval;
    std::string forward_mode;
    std::string monitor_mode;
    std::string status;
};


struct TamINTInfo
{
    std::string device_id;
    std::string status;
};


// collector
struct Collector {
    std::string ip;
    std::string port;
    std::string protocol;
};

// sampler
struct Sampler {
    std::string sampling_rate;
};

/* Collector map of tam, for multi collectors */
typedef std::map<std::string, Collector> TamCollectorMap;

/* Sampler map of tam */
typedef std::map<std::string, Sampler> TamSamplerMap;

class TamMgr : public Orch
{
public:
    TamMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tableNames, const std::string &type);

    using Orch::doTask;
private:
    Table                  m_cfgTamSwitchTable;
    Table                  m_cfgTamDMTable;
    Table                  m_cfgTamCollectorTable;
    Table                  m_cfgTamSamplerTable;
    Table                  m_cfgTamFeaturesTable;

    std::shared_ptr<DBConnector> m_counter_db;
    std::unique_ptr<Table>       m_counterTamInitTable;

    ProducerStateTable     m_appTamDMTable;
    ProducerStateTable     m_appTamCollectorTable;
    ProducerStateTable     m_appTamSamplerTable;
    ProducerStateTable     m_appTamINTable;

    // Memory info
    TamDMInfo              m_gTamDMInfo;
    TamCollectorMap        m_TamCollectorMap;
    TamSamplerMap          m_TamSamplerMap;
    TamINTInfo             m_gTamINTInfo;

    // TAM type, i.e., DM or INT
    std::string            m_gType;

    // TAM feature switch
    bool                   m_gDMEnable;
    bool                   m_gINTEnable;

    void doTask(Consumer &consumer);
    void tamHandleService(bool enable);
    void initTamCounterTable(bool enable);
    void tamGetDMInfo(std::vector<FieldValueTuple> &fvs, TamDMInfo &tam_global_info);
    void tamGetCollectorInfo(std::vector<FieldValueTuple> &fvs, Collector &collector);
    void tamGetSamplerInfo(std::vector<FieldValueTuple> &fvs, Sampler &sampler);
    void tamCheckDMInfoAndFillValues(std::vector<FieldValueTuple> &values, std::vector<FieldValueTuple> &fvs);
    void tamGetINTInfoAndFillValues(std::vector<FieldValueTuple> &fvs, TamINTInfo &tam_global_info);
    void tamCheckCollectorAndFillValues(std::string alias, std::vector<FieldValueTuple> &values,
                                       std::vector<FieldValueTuple> &fvs);
    void tamCheckSamplerAndFillValues(std::string alias, std::vector<FieldValueTuple> &values,
                                       std::vector<FieldValueTuple> &fvs);
};

}


