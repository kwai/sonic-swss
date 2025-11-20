#include <fstream>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "exec.h"
#include "tammgr.h"
#include "schema.h"
#include "select.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

#define TAM_TYPE_DM  "DM"
#define TAM_TYPE_INT "INT"

int main(int argc, char **argv)
{
    Logger::linkToDbNative("tammgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("--- Starting tammgrd ---");

    try
    {
        vector<string> cfg_dm_tables = {
            CFG_TAM_SWITCH_TABLE_NAME,
            CFG_TAM_DROPMONITOR_TABLE_NAME,
            CFG_TAM_COLLECTORS_TABLE_NAME,
            CFG_TAM_SAMPLINGRATE_TABLE_NAME,
            CFG_TAM_FEATURES_TABLE_NAME
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);

        /* Drop Monitor */
        TamMgr tammgr(&cfgDb, &appDb, cfg_dm_tables, TAM_TYPE_DM);

        vector<Orch *> cfgOrchList = {&tammgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        while (true)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_ERROR("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                tammgr.doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}
