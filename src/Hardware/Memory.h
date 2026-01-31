#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include <LittleFS.h>
void MemoryStartup()
{
    LittleFS.begin(true);
}

void MemoryReset()
{
    File Root = LittleFS.open("/", "r");
    String Delete = Root.getNextFileName();
    while (Delete.length() > 0)
    {
        LittleFS.remove(Delete);
        Delete = Root.getNextFileName();
    }
    Root.close();
}
#else
#include <lfs.h>
#endif
