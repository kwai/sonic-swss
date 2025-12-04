#pragma once

#include <map>
#include <string>
#include <inttypes.h>
#include <netinet/in.h>

#include "orch.h"
#include "portsorch.h"

using namespace swss;

extern MacAddress gMacAddress;

#define TAM_INT_FLEX_COUNTER_GROUP "TAM_INT_COUNTER"

struct TamDMEntry
{
    std::string switch_id;
    std::string aging_interval;
    bool        status;
};

struct TamCollectorEntry
{
    std::string ip;
    std::string port;
    std::string protocol;
};

struct TamSamplerEntry {
    std::string sampling_rate;
};


class TamOrch : public Orch
{
public:
    TamOrch(DBConnector *appDb, std::vector<std::string> &tableNames);

private:
    virtual void doTask(Consumer& consumer);
    void doIfaTask(string &op, string &key, vector<FieldValueTuple> &values);

    void tamStatusSet(Consumer &consumer);
    void tamExtractDMEntry(vector<FieldValueTuple> &fvs, TamDMEntry &tam_dm_entry);
    void tamCheckCollectorAndFillValues(string alias, vector<FieldValueTuple> &values);
    void tamCheckSamplerAndFillValues(string alias, vector<FieldValueTuple> &values);

    bool create_tam_report(sai_object_id_t* tam_report_id);
    bool create_tam_event_action(sai_object_id_t* tam_event_action_id);
    bool create_tam_transport(sai_object_id_t* tam_transport_id);
    bool create_tam_collector(sai_object_id_t* tam_collector_id);
    bool create_tam_event(sai_object_id_t* tam_event_id, int event_attr_type);
    bool create_tam(sai_object_id_t* tam_id, sai_object_id_t m_tam_event_packet_drop_id);

    bool create_hostif(sai_object_id_t* sai_hostif_obj);
    bool create_policer(sai_object_id_t* sai_policer_obj);
    bool create_hostif_trap_group(sai_object_id_t* sai_hostif_trap_group_obj);
    bool create_hostif_user_defined_trap(sai_object_id_t* sai_hostif_udt_obj);
    bool create_hostif_table_entry(sai_object_id_t* sai_hostif_table_entry_obj);

    bool enable_dm_set_port_attribute(vector<sai_object_id_t>& tam_oid_list);
    bool enable_dm_set_switch_attribute(vector<sai_object_id_t>& tam_oid_list);

    bool del_tam_report(sai_object_id_t tam_report_id);
    bool del_tam_event_action(sai_object_id_t tam_event_action_id);
    bool del_tam_transport(sai_object_id_t tam_transport_id);
    bool del_tam_collector(sai_object_id_t tam_collector_id);
    bool del_tam_event(sai_object_id_t tam_event_id);
    bool del_tam(sai_object_id_t tam_id);

    bool remove_hostif_table_entry(sai_object_id_t sai_hostif_table_entry_obj);
    bool remove_hostif_user_defined_trap(sai_object_id_t sai_hostif_udt_obj);
    bool remove_hostif_trap_group(sai_object_id_t sai_hostif_trap_group_obj);
    bool remove_policer(sai_object_id_t sai_policer_obj);
    bool remove_hostif(sai_object_id_t sai_hostif_obj);

    bool disable_dm_set_port_attribute();
    bool disable_dm_set_switch_attribute();


    bool create_tam_int_report(sai_object_id_t* tam_report_id);
    bool remove_tam_int_report(sai_object_id_t tam_report_id);


    bool tam_create_samplepacket(sai_object_id_t *samplepacket_id, int sample_rate, sai_samplepacket_type_t sampling_type);
    bool tam_remove_samplepacket(sai_object_id_t samplepacket_id);

    bool tam_create_tam_int(sai_object_id_t *tam_int_id, string &device_id, sai_object_id_t sample_packet_id, uint8_t l3_protocol);
    bool tam_remove_tam_int(sai_object_id_t tam_int_id);

    bool tam_create_acl_table(sai_object_id_t *acl_table_id);
    bool tam_remove_acl_table(sai_object_id_t acl_table_id);

    bool tam_create_acl_counter(sai_object_id_t *acl_counter_id, sai_object_id_t acl_table_id);
    bool tam_remove_acl_counter(sai_object_id_t acl_counter_id);

    bool tam_create_acl_entry(sai_object_id_t *acl_entry_id, sai_object_id_t acl_table_id, sai_object_id_t counter_id, sai_object_id_t tam_int_id, sai_object_id_t port_id, uint32_t priority);
    bool tam_remove_acl_entry(sai_object_id_t acl_entry_id);

    /* FlexCounter methods */
    void registerFlexCounter(const string &port, const sai_object_id_t counterOid);
    void deregisterFlexCounter(const string &port, const sai_object_id_t counterOid);

    /* MOD object ids */
    sai_object_id_t m_tam_report_id;
    sai_object_id_t m_tam_event_action_id;
    sai_object_id_t m_tam_transport_id;
    sai_object_id_t m_tam_collector_id;

    /* Marvell attribute*/
    sai_object_id_t m_sai_hostif_obj;
    sai_object_id_t m_sai_policer_obj;
    sai_object_id_t m_sai_hostif_trap_group_obj;
    sai_object_id_t m_sai_hostif_udt_obj;
    sai_object_id_t m_sai_hostif_table_entry_obj;

    // Ingress
    sai_object_id_t m_tam_ipp_event_packet_drop_id;
    sai_object_id_t m_tam_ipp_id;

    // MMU
    sai_object_id_t m_tam_mmu_event_packet_drop_id;
    sai_object_id_t m_tam_mmu_id;

    // Egress
    sai_object_id_t m_tam_epp_event_packet_drop_id;
    sai_object_id_t m_tam_epp_id;

    // Marvell mod event object, for marvell, SAI interface not distinguish stage
    sai_object_id_t m_tam_marvell_event_packet_drop_id;
    sai_object_id_t m_tam_marvell_id;

    sai_object_id_t m_cpu_mtp_id;
    //sai_object_id_t m_tam_port_id;

    TamDMEntry                m_TamDMEntry;
    TamCollectorEntry         m_TamCollectorEntry;
    TamSamplerEntry           m_TamSamplerEntry;

    /* INT object ids */
    sai_object_id_t m_samplepacket_id;
    sai_object_id_t m_tam_int_id;
    sai_object_id_t m_acl_table_id;
    sai_object_id_t m_tam_int_report_id;
    map<std::string, sai_object_id_t>  m_acl_entry_ids;
    map<std::string, sai_object_id_t>  m_acl_counter_ids;

    std::shared_ptr<swss::DBConnector> m_countersDb = nullptr;
    unique_ptr<Table> m_counterTable;

    FlexCounterManager m_flex_counter_manager;

    // to distinguish manufacturers, i.g., broadcom, clounix or marvell, etc
    std::string m_platform;
    // to use mod queue from sai.yaml on marvell platform
    int m_marvell_mod_queue;

};

