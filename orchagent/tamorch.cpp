#include "tamorch.h"

using namespace std;
using namespace swss;

extern PortsOrch*        gPortsOrch;
extern sai_object_id_t   gSwitchId;
extern sai_switch_api_t  *sai_switch_api;
extern sai_port_api_t    *sai_port_api;
extern sai_tam_api_t     *sai_tam_api;
extern sai_samplepacket_api_t*     sai_samplepacket_api;
extern sai_acl_api_t*              sai_acl_api;
extern sai_hostif_api_t*   sai_hostif_api;
extern sai_policer_api_t*  sai_policer_api;

#define TAM_TEST_MOD_DROPS_MAX    6
#define TAM_EVENT_ATTR_TYPE_IPP   0
#define TAM_EVENT_ATTR_TYPE_MMU   1
#define TAM_EVENT_ATTR_TYPE_EPP   2

#define TAM_INT_DEFAULT_POLLING_INTERVAL_MS 10000 // ms
#define COUNTERS_INT_PORT_RULE_MAP "COUNTERS_INT_PORT_MAP"

#define INT_PRESENCE_L3_PROTOCOL 0xfd
#define TAM_INT_ACL_PRIORITY 0xffffffff

// use cpu queue 46 as default
#define MRVL_DEFAULT_MOD_QUEUE    46

TamOrch::TamOrch(DBConnector *appDb, vector<string> &tableNames) :
        Orch(appDb, tableNames),
        m_flex_counter_manager(TAM_INT_FLEX_COUNTER_GROUP, StatsMode::READ, TAM_INT_DEFAULT_POLLING_INTERVAL_MS, false)
{
    SWSS_LOG_ENTER();

    m_TamDMEntry.status = false;
    m_countersDb = make_shared<DBConnector>("COUNTERS_DB", 0);
    m_counterTable = unique_ptr<Table>(new Table(m_countersDb.get(), COUNTERS_INT_PORT_RULE_MAP));

    string platform = getenv("ASIC_VENDOR") ? getenv("ASIC_VENDOR") : "";
    if (platform == "")
    {
        SWSS_LOG_WARN("Platform environment variable is not defined");
    }
    else
    {
        m_platform = platform;
    }
    string marvell_mod_queue = getenv("marvell_mod_queue") ? getenv("marvell_mod_queue") : "";
    if (marvell_mod_queue == "")
    {
        m_marvell_mod_queue = MRVL_DEFAULT_MOD_QUEUE;
    }
    else
    {
        try
        {
            m_marvell_mod_queue = stoi(marvell_mod_queue);
        }
        catch(...)
        {
            SWSS_LOG_ERROR("Invalid parameter for marvell MOD_QUEUE: %s, use default.", marvell_mod_queue.c_str());
            m_marvell_mod_queue = MRVL_DEFAULT_MOD_QUEUE;
        }
    }

    SWSS_LOG_NOTICE("tamorch init");
}

bool TamOrch::tam_create_samplepacket(sai_object_id_t *samplepacket_id, int sample_rate, sai_samplepacket_type_t sampling_type = SAI_SAMPLEPACKET_TYPE_SLOW_PATH)
{
    vector<sai_attribute_t> attr_list;
    sai_attribute_t sample_attr;

    sample_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
    sample_attr.value.u32 = sample_rate;
    attr_list.emplace_back(sample_attr);

    sample_attr.id = SAI_SAMPLEPACKET_ATTR_TYPE;
    sample_attr.value.s32 = sampling_type;
    attr_list.emplace_back(sample_attr);

    sample_attr.id = SAI_SAMPLEPACKET_ATTR_MODE;
    sample_attr.value.s32 = SAI_SAMPLEPACKET_MODE_EXCLUSIVE;
    attr_list.emplace_back(sample_attr);

    sai_status_t status = sai_samplepacket_api->create_samplepacket(samplepacket_id, gSwitchId, (uint32_t)attr_list.size(), attr_list.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create samplepacket, rv:%d", status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_SAMPLEPACKET, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_remove_samplepacket(sai_object_id_t samplepacket_id)
{
    sai_status_t sai_rc = sai_samplepacket_api->remove_samplepacket(samplepacket_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s INT sample packet object with id %" PRIx64 "", m_platform.c_str(), samplepacket_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_SAMPLEPACKET, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_tam_int_report(sai_object_id_t* tam_report_id)
{
    sai_attribute_t tam_attr;

    tam_attr.id = SAI_TAM_REPORT_ATTR_TYPE;
    tam_attr.value.s32 = SAI_TAM_REPORT_TYPE_IPFIX;

    sai_status_t sai_rc = sai_tam_api->create_tam_report(tam_report_id, gSwitchId, 1, &tam_attr);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s INT report, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::remove_tam_int_report(sai_object_id_t tam_report_id)
{
    sai_status_t sai_rc = sai_tam_api->remove_tam_report(tam_report_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s INT report object, status: %s", m_platform.c_str(), sai_serialize_status(sai_rc).c_str());
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_create_tam_int(sai_object_id_t *tam_int_id, string &device_id, sai_object_id_t sample_packet_id, uint8_t l3_protocol)
{
    sai_status_t sai_rc = 0;
    struct in_addr ipv4_addr;

    if (inet_pton(AF_INET, device_id.c_str(), &ipv4_addr) != 1)
    {
        SWSS_LOG_ERROR("Invalid TAM device id: %s", device_id.c_str());
        return false;
    }

    SWSS_LOG_INFO("type %d, device_id(%s): %d, l3_protocol %d", SAI_TAM_INT_TYPE_IFA2, device_id.c_str(), htonl(ipv4_addr.s_addr), l3_protocol);

    vector<sai_attribute_t> attr_list;
    sai_attribute_t attr;

    attr.id = SAI_TAM_INT_ATTR_TYPE;
    attr.value.s32 = SAI_TAM_INT_TYPE_IFA2;
    attr_list.emplace_back(attr);

    attr.id = SAI_TAM_INT_ATTR_INT_PRESENCE_TYPE;
    attr.value.s32 = SAI_TAM_INT_PRESENCE_TYPE_L3_PROTOCOL;
    attr_list.emplace_back(attr);

    attr.id = SAI_TAM_INT_ATTR_DEVICE_ID;
    attr.value.u32 = htonl(ipv4_addr.s_addr);
    attr_list.emplace_back(attr);

    attr.id = SAI_TAM_INT_ATTR_INLINE;
    attr.value.booldata = true;
    attr_list.emplace_back(attr);

    attr.id = SAI_TAM_INT_ATTR_INT_PRESENCE_L3_PROTOCOL;
    attr.value.u8 = INT_PRESENCE_L3_PROTOCOL;
    attr_list.emplace_back(attr);

    attr.id = SAI_TAM_INT_ATTR_NAME_SPACE_ID;
    attr.value.u8 = 0x6;
    attr_list.emplace_back(attr);

    attr.id = SAI_TAM_INT_ATTR_NAME_SPACE_ID_GLOBAL;
    attr.value.booldata = false;
    attr_list.emplace_back(attr);

    attr.id = SAI_TAM_INT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = sample_packet_id;
    attr_list.emplace_back(attr);

    attr.id = SAI_TAM_INT_ATTR_REPORT_ID;
    attr.value.oid = m_tam_int_report_id;
    attr_list.emplace_back(attr);

    sai_rc = sai_tam_api->create_tam_int(tam_int_id, gSwitchId, (uint32_t)attr_list.size(), attr_list.data());
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s INT object, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_remove_tam_int(sai_object_id_t tam_int_id)
{
    sai_status_t sai_rc = sai_tam_api->remove_tam_int(tam_int_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove %s INT object, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_create_acl_table(sai_object_id_t *acl_table_id)
{
    sai_attribute_t attr_list[10];
    uint32_t attr_count = 0;
    int32_t action_list[4];
    uint32_t action_count = 0;
    sai_status_t sai_rc = 0;

    uint32_t arr_idx = 0;
    int32_t bindpoint_type_arr[10];
    sai_s32_list_t sai_bindpoint_type_list;

    /* Mandatory Params */
    attr_list[attr_count].value.u32 = SAI_ACL_STAGE_EGRESS;
    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    attr_count++;

    bindpoint_type_arr[arr_idx++] = SAI_ACL_BIND_POINT_TYPE_SWITCH;
    sai_bindpoint_type_list.list = bindpoint_type_arr;
    sai_bindpoint_type_list.count = arr_idx;

    attr_list[attr_count].value.s32list = sai_bindpoint_type_list;
    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST;
    attr_count++;

    /* Optional Params */
    attr_list[attr_count].value.u32 = 256;
    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_SIZE;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE;
    attr_list[attr_count].value.booldata = true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_FRAG;
    attr_list[attr_count].value.booldata = true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL;
    attr_list[attr_count].value.booldata = true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_FIELD_TAM_INT_TYPE;
    attr_list[attr_count].value.booldata = true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_FIELD_OUT_PORT;
    attr_list[attr_count].value.booldata = true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_TABLE_ATTR_ACL_ACTION_TYPE_LIST;
    attr_list[attr_count].value.s32list.list = action_list;
    action_list[action_count++] = SAI_ACL_ACTION_TYPE_INT_INSERT;
    action_list[action_count++] = SAI_ACL_ACTION_TYPE_COUNTER;
    attr_list[attr_count].value.s32list.count = action_count;
    attr_count++;

    sai_rc = sai_acl_api->create_acl_table(acl_table_id, gSwitchId, attr_count, attr_list);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s INT acl table object, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_remove_acl_table(sai_object_id_t acl_table_id)
{
    sai_status_t sai_rc = sai_acl_api->remove_acl_table(acl_table_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove %s INT acl table object, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_create_acl_counter(sai_object_id_t *acl_counter_id, sai_object_id_t acl_table_id)
{
    sai_attribute_t attr_list[8];
    uint32_t attr_count = 0;
    attr_list[attr_count].id = SAI_ACL_COUNTER_ATTR_TABLE_ID;
    attr_list[attr_count].value.oid = acl_table_id;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_COUNTER_ATTR_ENABLE_PACKET_COUNT;
    attr_list[attr_count].value.booldata = true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_COUNTER_ATTR_ENABLE_BYTE_COUNT;
    attr_list[attr_count].value.booldata = true;
    attr_count++;

    sai_status_t sai_rc = sai_acl_api->create_acl_counter(acl_counter_id, gSwitchId, attr_count, attr_list);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s INT acl counter object, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_remove_acl_counter(sai_object_id_t acl_counter_id)
{
    sai_status_t sai_rc = sai_acl_api->remove_acl_counter(acl_counter_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove %s INT acl counter object, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_create_acl_entry(sai_object_id_t *acl_entry_id,
                                   sai_object_id_t acl_table_id,
                                   sai_object_id_t counter_id,
                                   sai_object_id_t tam_int_id,
                                   sai_object_id_t port_id,
                                   uint32_t priority)
{
    sai_attribute_t attr_list[10];
    uint32_t attr_count = 0;
    sai_status_t sai_rc = 0;

    /* Mandatory Params */
    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    attr_list[attr_count].value.oid = acl_table_id;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    attr_list[attr_count].value.u32 = TAM_INT_ACL_PRIORITY;
    attr_count++;

    /* Other params */
    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_ACTION_COUNTER;
    attr_list[attr_count].value.aclaction.parameter.oid = counter_id;
    attr_list[attr_count].value.aclaction.enable= true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_ACTION_TAM_INT_OBJECT;
    attr_list[attr_count].value.aclaction.parameter.oid = tam_int_id;
    attr_list[attr_count].value.aclaction.enable= true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_ACTION_INT_INSERT;
    attr_list[attr_count].value.aclaction.parameter.booldata = true;
    attr_list[attr_count].value.aclaction.enable= true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORT;
    attr_list[attr_count].value.aclfield.data.oid = port_id;
    attr_list[attr_count].value.aclfield.enable= true;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_FIELD_TAM_INT_TYPE;
    attr_list[attr_count].value.aclfield.enable = true;
    attr_list[attr_count].value.aclfield.data.s32 = SAI_TAM_INT_TYPE_IFA2;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE;
    attr_list[attr_count].value.aclfield.data.u32 = SAI_ACL_IP_TYPE_IPV4ANY;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL;
    attr_list[attr_count].value.aclfield.data.u8 = 17;    //UDP
    attr_list[attr_count].value.aclfield.mask.u8 = 0xFF;
    attr_count++;

    attr_list[attr_count].id = SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_FRAG;
    attr_list[attr_count].value.aclfield.data.u32 = SAI_ACL_IP_FRAG_NON_FRAG;
    attr_count++;

    sai_rc = sai_acl_api->create_acl_entry(acl_entry_id, gSwitchId, attr_count, attr_list);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s INT acl entry, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::tam_remove_acl_entry(sai_object_id_t acl_entry_id)
{
    sai_status_t sai_rc = sai_acl_api->remove_acl_entry(acl_entry_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove %s INT acl entry, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

void TamOrch::doIfaTask(string &op, string &key, vector<FieldValueTuple> &values)
{
    sai_object_id_t acl_counter_id;
    sai_object_id_t acl_entry_id;

    SWSS_LOG_ENTER();

    if (op == SET_COMMAND)
    {
        string device_id = "";
        string status = "";

        for (auto i : values)
        {
            if (fvField(i) == "status")
            {
                status = fvValue(i);
            }
            else if (fvField(i) == "device-id")
            {
                device_id = fvValue(i);
            }
        }

        SWSS_LOG_INFO("status %s, device_id:%s", status.c_str(), device_id.c_str());

        if (status == "ACTIVE")
        {
            // Create IFA config
            // 1、Create samplepacket
            tam_create_samplepacket(&m_samplepacket_id, 1, SAI_SAMPLEPACKET_TYPE_SLOW_PATH);
            SWSS_LOG_INFO("create_samplepacket done, samplepacket_id: 0x%lx", m_samplepacket_id);

            create_tam_int_report(&m_tam_int_report_id);

            // 2、create tam INT object
            tam_create_tam_int(&m_tam_int_id, device_id, m_samplepacket_id, 0xfd);
            SWSS_LOG_INFO("tam_create_tam_int done, tam_int_id: 0x%lx", m_tam_int_id);

            // 3、create acl table
            tam_create_acl_table(&m_acl_table_id);
            SWSS_LOG_INFO("tam_create_acl_table done, acl_table_id: 0x%lx", m_acl_table_id);

            for (auto const &curr : gPortsOrch->getAllPorts())
            {
                acl_counter_id = SAI_NULL_OBJECT_ID;
                acl_entry_id = SAI_NULL_OBJECT_ID;
                if (curr.second.m_port_id == SAI_NULL_OBJECT_ID || curr.second.m_type != Port::Type::PHY)
                {
                    SWSS_LOG_NOTICE("Skipping port(%s) type:%d, oid: 0x%lx", curr.first.c_str(), curr.second.m_type, curr.second.m_port_id);
                    continue;
                }

                // 4、create acl counter
                tam_create_acl_counter(&acl_counter_id, m_acl_table_id);
                SWSS_LOG_INFO("tam_create_acl_counter done, acl_counter_id: 0x%lx", acl_counter_id);
                m_acl_counter_ids[curr.first] = acl_counter_id;

                // 5、create acl entry
                tam_create_acl_entry(&acl_entry_id, m_acl_table_id, acl_counter_id, m_tam_int_id, curr.second.m_port_id, 1);
                SWSS_LOG_INFO("tam_create_acl_entry done, acl_entry_id: 0x%lx", acl_entry_id);
                m_acl_entry_ids[curr.first] = acl_entry_id;

                registerFlexCounter(curr.first, acl_counter_id);

                SWSS_LOG_INFO("tam_create_acl_entry done, m_port_id(%s): 0x%lx, counter_id: 0x%lx, acl_entry_id: 0x%lx", curr.first.c_str(), curr.second.m_port_id, acl_counter_id, acl_entry_id);
            }

            m_flex_counter_manager.enableFlexCounterGroup();
        }
        else if (status == "INACTIVE")
        {
            m_flex_counter_manager.disableFlexCounterGroup();

            // TODO:Remove IFA config
            for (auto const &curr : gPortsOrch->getAllPorts())
            {
                if (curr.second.m_type != Port::Type::PHY ||
                    m_acl_entry_ids.find(curr.first) == m_acl_entry_ids.end())
                {
                    SWSS_LOG_NOTICE("Skipping port(%s) type:%d, oid: 0x%lx", curr.first.c_str(), curr.second.m_type, curr.second.m_port_id);
                    continue;
                }

                deregisterFlexCounter(curr.first, m_acl_counter_ids[curr.first]);

                // 1、remove acl entry
                tam_remove_acl_entry(m_acl_entry_ids[curr.first]);

                // 2、remove acl counter
                tam_remove_acl_counter(m_acl_counter_ids[curr.first]);
            }

            // 3、remove acl table
            tam_remove_acl_table(m_acl_table_id);
            SWSS_LOG_INFO("tam_remove_acl_table done, acl_table_id: 0x%lx", m_acl_table_id);

            // 4、remove tam INT object
            tam_remove_tam_int(m_tam_int_id);
            SWSS_LOG_INFO("tam_remove_tam_int done, tam_int_id: 0x%lx", m_tam_int_id);

            remove_tam_int_report(m_tam_int_report_id);
            SWSS_LOG_INFO("tam_remove_tam_report done, tam_int_report_id: 0x%lx", m_tam_int_report_id);

            // 5、remove samplepacket
            tam_remove_samplepacket(m_samplepacket_id);
            SWSS_LOG_INFO("tam_remove_samplepacket done, samplepacket_id: 0x%lx", m_samplepacket_id);
        }
    }
    else if (op == DEL_COMMAND)
    {
        SWSS_LOG_ERROR("TODO: DEL_COMMAND Not Support");
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
    }
}

void TamOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    Port   port;
    string table_name = consumer.getTableName();

    SWSS_LOG_NOTICE("tamorch: doTask");

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple tuple = it->second;

        string op = kfvOp(tuple);
        string key = kfvKey(tuple);
        auto values = kfvFieldsValues(tuple);

        SWSS_LOG_DEBUG("Table name is: %s", table_name.c_str());

        if (op == SET_COMMAND)
        {
            if (table_name == APP_TAM_COLLECTOR_TABLE)
            {
                tamCheckCollectorAndFillValues(key, values);
            }
            else if (table_name == APP_TAM_SAMPLER_TABLE)
            {
                tamCheckSamplerAndFillValues(key, values);
            }

            // only APPL_DB "TAM_DROPMONITOR:gloabl" field "status" changed, then update the configuration of SAI/SDK
            // the update of the attribute is invalid until "status" is disabled and re-enabled
            else if (table_name == APP_TAM_DROPMONITOR_TABLE)
            {
                TamDMEntry tam_dm_entry = {
                    m_TamDMEntry.switch_id,
                    m_TamDMEntry.aging_interval,
                    m_TamDMEntry.status
                };

                SWSS_LOG_NOTICE("Original tam drop monitor status is %d", tam_dm_entry.status);

                tamExtractDMEntry(kfvFieldsValues(tuple), tam_dm_entry);

                if (tam_dm_entry.switch_id != m_TamDMEntry.switch_id)
                {
                    m_TamDMEntry.switch_id = tam_dm_entry.switch_id;
                }

                if (tam_dm_entry.aging_interval != m_TamDMEntry.aging_interval)
                {
                    m_TamDMEntry.aging_interval = tam_dm_entry.aging_interval;
                }

                if (tam_dm_entry.status != m_TamDMEntry.status)
                {
                    if (tam_dm_entry.status)
                    {
                        if (m_TamSamplerEntry.sampling_rate.empty())
                        {
                            it++;
                            continue;
                        }

                        SWSS_LOG_NOTICE("Start set asic chip configuration.");
                        m_TamDMEntry.status = true;
                        SWSS_LOG_NOTICE("Now tam drop monitor status is %d", m_TamDMEntry.status);

                        if (!create_tam_report(&m_tam_report_id) ||
                            !create_tam_event_action(&m_tam_event_action_id) ||
                            !create_tam_transport(&m_tam_transport_id))
                        {
                            it++;
                            continue;
                        }

                        if (!create_tam_collector(&m_tam_collector_id))
                        {
                            it++;
                            continue;
                        }

                        if (m_platform == MRVL_TL_PLATFORM_SUBSTRING)
                        {
                            if (!create_tam_event(&m_tam_marvell_event_packet_drop_id, TAM_EVENT_ATTR_TYPE_IPP))
                            {
                                it++;
                                continue;
                            }

                            if (!create_tam(&m_tam_marvell_id, m_tam_marvell_event_packet_drop_id))
                            {
                                it++;
                                continue;
                            }

                            vector<sai_object_id_t> tam_oid_list;
                            tam_oid_list.push_back(m_tam_marvell_id);

                            if (!enable_dm_set_switch_attribute(tam_oid_list))
                            {
                                it++;
                                continue;
                            }
                        }

                        SWSS_LOG_NOTICE("End of set asic chip configuration.");
                    }
                    else
                    {
                        SWSS_LOG_NOTICE("Start delete asic chip configuration.");
                        m_TamDMEntry.status = false;
                        SWSS_LOG_NOTICE("Now tam drop monitor status is %d", m_TamDMEntry.status);

                        if (!disable_dm_set_switch_attribute())
                        {
                            it++;
                            continue;
                        }

                        if (!disable_dm_set_port_attribute())
                        {
                            it++;
                            continue;
                        }

                        if (m_platform == MRVL_TL_PLATFORM_SUBSTRING)
                        {
                            if (!del_tam(m_tam_marvell_id))
                            {
                                it++;
                                continue;
                            }

                            if (!del_tam_event(m_tam_marvell_event_packet_drop_id))
                            {
                                it++;
                                continue;
                            }
                        }

                        if (!del_tam_collector(m_tam_collector_id))
                        {
                            it++;
                            continue;
                        }

                        if (!remove_policer(m_sai_policer_obj))
                        {
                            it++;
                            continue;
                        }

                        if (!del_tam_transport(m_tam_transport_id))
                        {
                            it++;
                            continue;
                        }

                        if (!del_tam_event_action(m_tam_event_action_id))
                        {
                            it++;
                            continue;
                        }
                        if (!del_tam_report(m_tam_report_id))
                        {
                            it++;
                            continue;
                        }

                        SWSS_LOG_NOTICE("End delete asic chip configuration.");
                    }
                }

            }
            else if (table_name == APP_TAM_INT_TABLE)
            {
                doIfaTask(op, key, values);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

// Store info in the memory structure TamSamplerEntry, and fill fvs to update appl_db
void TamOrch::tamCheckSamplerAndFillValues(string alias, vector<FieldValueTuple> &values)
{
    for (auto i : values)
    {
        SWSS_LOG_DEBUG("fvField is %s, fvValue is %s", fvField(i).c_str(), fvValue(i).c_str());

        if (fvField(i) == "sampling-rate")
        {
            m_TamSamplerEntry.sampling_rate = fvValue(i);
        }
    }
}

// Store info in the memory structure TamCollectorEntry, and fill fvs to update appl_db
void TamOrch::tamCheckCollectorAndFillValues(string alias, vector<FieldValueTuple> &values)
{
    for (auto i : values)
    {
        SWSS_LOG_DEBUG("fvField is %s, fvValue is %s", fvField(i).c_str(), fvValue(i).c_str());

        if (fvField(i) == "ip")
        {
            m_TamCollectorEntry.ip = fvValue(i);
        }
        if (fvField(i) == "port")
        {
            m_TamCollectorEntry.port = fvValue(i);
        }
        if (fvField(i) == "protocol")
        {
            m_TamCollectorEntry.protocol = fvValue(i);
        }
    }
}

void TamOrch::tamExtractDMEntry(vector<FieldValueTuple> &fvs, TamDMEntry &tam_dm_entry)
{
    for (auto i : fvs)
    {
        if (fvField(i) == "status")
        {
            if (fvValue(i) == "ACTIVE")
            {
                tam_dm_entry.status = true;
            }
            else if (fvValue(i) == "INACTIVE")
            {
                tam_dm_entry.status = false;
            }
        }
        else if (fvField(i) == "switch-id")
        {
            tam_dm_entry.switch_id = fvValue(i);
        }
        else if (fvField(i) == "aging-interval")
        {
            tam_dm_entry.aging_interval = fvValue(i);
        }
    }
}

bool TamOrch::create_tam_report(sai_object_id_t* tam_report_id)
{
    sai_attribute_t tam_attr_list[10] = {};
    uint32_t count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_TAM_REPORT_ATTR_TYPE;
    tam_attr_list[count].value.s32 = SAI_TAM_REPORT_TYPE_GENETLINK;
    count++;

    sai_rc = sai_tam_api->create_tam_report(tam_report_id, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD report, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_tam_event_action(sai_object_id_t* tam_event_action_id)
{
    sai_attribute_t tam_attr_list[10] = {};
    uint32_t count = 0;

    tam_attr_list[count].id = SAI_TAM_EVENT_ACTION_ATTR_REPORT_TYPE;
    tam_attr_list[count].value.oid = m_tam_report_id;
    count++;

    sai_status_t sai_rc = sai_tam_api->create_tam_event_action(tam_event_action_id, gSwitchId, count, tam_attr_list);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD event action, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_tam_transport(sai_object_id_t* tam_transport_id)
{
    sai_attribute_t tam_attr_list[10] = {};
    uint32_t count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_TAM_TRANSPORT_ATTR_TRANSPORT_TYPE;
    tam_attr_list[count].value.s32 =  SAI_TAM_TRANSPORT_TYPE_NONE;
    count++;

    sai_rc = sai_tam_api->create_tam_transport(tam_transport_id, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD transport, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_tam_collector(sai_object_id_t* tam_collector_id)
{
    sai_attribute_t tam_attr_list[10] = {};
    uint32_t count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_TAM_COLLECTOR_ATTR_TRANSPORT;
    tam_attr_list[count].value.oid = m_tam_transport_id;
    count++;

    tam_attr_list[count].id = SAI_TAM_COLLECTOR_ATTR_HOSTIF_TRAP;
    tam_attr_list[count].value.oid = m_sai_hostif_udt_obj;
    count++;

    tam_attr_list[count].id = SAI_TAM_COLLECTOR_ATTR_DSCP_VALUE;
    tam_attr_list[count].value.u8 = 0;
    count++;

    tam_attr_list[count].id = SAI_TAM_COLLECTOR_ATTR_SRC_IP;
    tam_attr_list[count].value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    tam_attr_list[count].value.ipaddr.addr.ip4 = htonl(0x01010101);
    count++;

    tam_attr_list[count].id = SAI_TAM_COLLECTOR_ATTR_DST_IP;
    tam_attr_list[count].value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    tam_attr_list[count].value.ipaddr.addr.ip4 = htonl(0x01010102);
    count++;

    sai_rc = sai_tam_api->create_tam_collector(tam_collector_id, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD collector, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_tam_event(sai_object_id_t* tam_event_id, int event_attr_type)
{
    sai_attribute_t tam_attr_list[10] = {};
    int count;
    sai_status_t sai_rc = 0;

    tam_attr_list[0].id = SAI_TAM_EVENT_ATTR_TYPE;
    tam_attr_list[0].value.s32 =  SAI_TAM_EVENT_TYPE_PACKET_DROP;

    tam_attr_list[1].id = SAI_TAM_EVENT_ATTR_ACTION_LIST;
    tam_attr_list[1].value.objlist.count = 1;
    tam_attr_list[1].value.objlist.list = (sai_object_id_t *)calloc(1, sizeof(sai_object_id_t));
    tam_attr_list[1].value.objlist.list[0] = m_tam_event_action_id;

    tam_attr_list[2].id = SAI_TAM_EVENT_ATTR_COLLECTOR_LIST;
    tam_attr_list[2].value.objlist.count = 1;
    tam_attr_list[2].value.objlist.list = (sai_object_id_t *)calloc(1, sizeof(sai_object_id_t));
    tam_attr_list[2].value.objlist.list[0] = m_tam_collector_id;

    count = 3;
    sai_rc = sai_tam_api->create_tam_event(tam_event_id, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD  event object, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_tam(sai_object_id_t* tam_id, sai_object_id_t tam_event_packet_drop_id)
{
    sai_attribute_t tam_attr_list[10] = {};
    uint32_t count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_TAM_ATTR_EVENT_OBJECTS_LIST;
    tam_attr_list[count].value.objlist.list = (sai_object_id_t *)calloc(1, sizeof(sai_object_id_t));
    tam_attr_list[count].value.objlist.list[0] = tam_event_packet_drop_id;
    tam_attr_list[count].value.objlist.count = 1;
    count++;

    tam_attr_list[count].id =  SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST;
    tam_attr_list[count].value.s32list.list = (sai_int32_t *)calloc(2, sizeof(sai_int32_t));
    tam_attr_list[count].value.s32list.list[0] = SAI_TAM_BIND_POINT_TYPE_SWITCH;
    tam_attr_list[count].value.s32list.count = 1;
    count++;

    sai_rc = sai_tam_api->create_tam(tam_id, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create  %s MOD tam object, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::enable_dm_set_port_attribute(vector<sai_object_id_t>& tam_oid_list)
{
    return true;
}

bool TamOrch::enable_dm_set_switch_attribute(vector<sai_object_id_t>& tam_oid_list)
{
    sai_attribute_t switch_attr = {};
    switch_attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
    switch_attr.value.objlist.count = (uint32_t)tam_oid_list.size();
    switch_attr.value.objlist.list = tam_oid_list.data();

    sai_status_t sai_rc = sai_switch_api->set_switch_attribute(gSwitchId, &switch_attr);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set %s MOD switch attribute, sai_rc:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, sai_rc);
        if (handle_status != task_process_status::task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_hostif(sai_object_id_t* sai_hostif_obj)
{
    sai_attribute_t tam_attr_list[10] = {};
    uint32_t count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_HOSTIF_ATTR_TYPE;
    tam_attr_list[count].value.s32 =  SAI_HOSTIF_TYPE_GENETLINK;
    count++;

    tam_attr_list[count].id = SAI_HOSTIF_ATTR_NAME; //GENETLINK family name
    strncpy(tam_attr_list[count].value.chardata, "NET_DM", 31);
    tam_attr_list[count].value.chardata[31] = '\0';
    count++;

    tam_attr_list[count].id = SAI_HOSTIF_ATTR_GENETLINK_MCGRP_NAME;
    strncpy(tam_attr_list[count].value.chardata, "events", 31);
    tam_attr_list[count].value.chardata[31] = '\0';
    count++;

    sai_rc = sai_hostif_api->create_hostif(sai_hostif_obj, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD hostif, sai_rc:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_policer(sai_object_id_t* sai_policer_obj)
{
    sai_attribute_t tam_attr_list[10] = {};
    uint32_t count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_POLICER_ATTR_METER_TYPE;
    tam_attr_list[count].value.s32 = SAI_METER_TYPE_PACKETS;
    count++;

    tam_attr_list[count].id = SAI_POLICER_ATTR_MODE;
    tam_attr_list[count].value.s32 =  SAI_POLICER_MODE_SR_TCM;
    count++;

    tam_attr_list[count].id = SAI_POLICER_ATTR_CBS;
    tam_attr_list[count].value.s32 = 2000;
    count++;

    tam_attr_list[count].id = SAI_POLICER_ATTR_CIR;
    tam_attr_list[count].value.s32 = 1000; //rate limit MOD packets to CPU
    count++;

    tam_attr_list[count].id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
    tam_attr_list[count].value.s32 = SAI_PACKET_ACTION_DROP;
    count++;

    sai_rc = sai_policer_api->create_policer(sai_policer_obj, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD policer, sai_rc:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_hostif_trap_group(sai_object_id_t* sai_hostif_trap_group_obj)
{
    sai_attribute_t tam_attr_list[10] = {};
    int count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE;
    tam_attr_list[count].value.s32 = m_marvell_mod_queue;
    count++;

    tam_attr_list[count].id = SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER;
    tam_attr_list[count].value.oid = m_sai_policer_obj;
    count++;

    sai_rc = sai_hostif_api->create_hostif_trap_group(sai_hostif_trap_group_obj, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD hostif trap group, sai_rc:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_hostif_user_defined_trap(sai_object_id_t* sai_hostif_udt_obj)
{
    sai_attribute_t tam_attr_list[10] = {};
    uint32_t count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TYPE;
    tam_attr_list[count].value.s32 = SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_TAM;
    count++;

    tam_attr_list[count].id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TRAP_GROUP;
    tam_attr_list[count].value.oid = m_sai_hostif_trap_group_obj;
    count++;

    sai_rc = sai_hostif_api->create_hostif_user_defined_trap(sai_hostif_udt_obj, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD hostif user defined trap, sai_rc:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::create_hostif_table_entry(sai_object_id_t* sai_hostif_table_entry_obj)
{
    sai_attribute_t tam_attr_list[10] = {};
    int count = 0;
    sai_status_t sai_rc = 0;

    tam_attr_list[count].id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TYPE;
    tam_attr_list[count].value.s32 =  SAI_HOSTIF_TABLE_ENTRY_TYPE_TRAP_ID;
    count++;

    tam_attr_list[count].id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TRAP_ID;
    tam_attr_list[count].value.oid = m_sai_hostif_udt_obj;
    count++;

    tam_attr_list[count].id = SAI_HOSTIF_TABLE_ENTRY_ATTR_CHANNEL_TYPE;
    tam_attr_list[count].value.s32 = SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_GENETLINK;
    count++;

    tam_attr_list[count].id = SAI_HOSTIF_TABLE_ENTRY_ATTR_HOST_IF;
    tam_attr_list[count].value.oid = m_sai_hostif_obj;
    count++;

    sai_rc = sai_hostif_api->create_hostif_table_entry(sai_hostif_table_entry_obj, gSwitchId, count, tam_attr_list);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s MOD hostif table entry, sai_rc:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::del_tam_report(sai_object_id_t tam_report_id)
{
    sai_status_t sai_rc = sai_tam_api->remove_tam_report(tam_report_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD report, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::del_tam_event_action(sai_object_id_t tam_event_action_id)
{
    sai_status_t sai_rc = sai_tam_api->remove_tam_event_action(tam_event_action_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD event action, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::del_tam_transport(sai_object_id_t tam_transport_id)
{
    sai_status_t sai_rc = sai_tam_api->remove_tam_transport(tam_transport_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD transport, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::del_tam_collector(sai_object_id_t tam_collector_id)
{
    sai_status_t sai_rc = sai_tam_api->remove_tam_collector(tam_collector_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD collector, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::del_tam_event(sai_object_id_t tam_event_id)
{
    sai_status_t sai_rc = sai_tam_api->remove_tam_event(tam_event_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD event, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::del_tam(sai_object_id_t tam_id)
{
    sai_status_t sai_rc = sai_tam_api->remove_tam(tam_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD tam, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::disable_dm_set_port_attribute()
{
    return true;
}

bool TamOrch::disable_dm_set_switch_attribute()
{
    sai_attribute_t switch_attr;

    switch_attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
    switch_attr.value.objlist.count = 0;

    sai_status_t sai_rc = sai_switch_api->set_switch_attribute(gSwitchId, &switch_attr);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD switch attribute, rv:%d.", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, sai_rc);
        if (handle_status != task_process_status::task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::remove_hostif_table_entry(sai_object_id_t sai_hostif_table_entry_obj)
{
    sai_status_t sai_rc = sai_hostif_api->remove_hostif_table_entry(sai_hostif_table_entry_obj);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD hostif table entry, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return  true;
}

bool TamOrch::remove_hostif_user_defined_trap(sai_object_id_t sai_hostif_udt_obj)
{
    sai_status_t sai_rc = sai_hostif_api->remove_hostif_user_defined_trap(sai_hostif_udt_obj);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD hostif user defined trap, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::remove_hostif_trap_group(sai_object_id_t sai_hostif_trap_group_obj)
{
    sai_status_t sai_rc = sai_hostif_api->remove_hostif_trap_group(sai_hostif_trap_group_obj);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD hostif trap group, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::remove_policer(sai_object_id_t sai_policer_obj)
{
    sai_status_t sai_rc = sai_policer_api->remove_policer(sai_policer_obj);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD policer, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool TamOrch::remove_hostif(sai_object_id_t sai_hostif_obj)
{
    sai_status_t sai_rc = sai_hostif_api->remove_hostif(sai_hostif_obj);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete %s MOD hostif, rv:%d", m_platform.c_str(), sai_rc);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

static map<sai_acl_counter_attr_t, sai_acl_counter_attr_t> aclTamIntCounterLookup =
{
    {SAI_ACL_COUNTER_ATTR_ENABLE_BYTE_COUNT,   SAI_ACL_COUNTER_ATTR_BYTES},
    {SAI_ACL_COUNTER_ATTR_ENABLE_PACKET_COUNT, SAI_ACL_COUNTER_ATTR_PACKETS},
};

void TamOrch::registerFlexCounter(const string &port, const sai_object_id_t counterOid)
{
    SWSS_LOG_ENTER();

    unordered_set<string> serializedCounterStatAttrs;
    for (const auto& counterAttrPair: aclTamIntCounterLookup)
    {
        sai_acl_counter_attr_t id {};
        tie(std::ignore, id) = counterAttrPair;
        auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_COUNTER, id);
        if (!meta)
        {
            SWSS_LOG_THROW("SAI Bug: Failed to get metadata of attribute %d for SAI_OBJECT_TYPE_ACL_COUNTER", id);
        }
        serializedCounterStatAttrs.insert(sai_serialize_attr_id(*meta));
    }

    m_flex_counter_manager.setCounterIdList(counterOid, CounterType::TAM_INT, serializedCounterStatAttrs);

    FieldValueTuple tuple(port, sai_serialize_object_id(counterOid));
    vector<FieldValueTuple> fields;
    fields.push_back(tuple);
    m_counterTable->set("", fields);

}

void TamOrch::deregisterFlexCounter(const string &port, const sai_object_id_t counterOid)
{
    SWSS_LOG_ENTER();
    /* remove port name map from counter table */
    m_counterTable->hdel("", port);
    m_flex_counter_manager.clearCounterIdList(counterOid);
}
