#include <SPIFFS.h>

void MemoryStartup()
{
    SPIFFS.begin(true);
}

void MemoryReset()
{
    File Root = SPIFFS.open("/", "r");
    String Delete = Root.getNextFileName();
    while (Delete.length() > 0)
    {
        SPIFFS.remove(Delete);
        Delete = Root.getNextFileName();
    }
    Root.close();
}
