An index of every command ID, grouped by the section that owns it. The payloads and the behaviour live in the owning service document.

| Section | Command | ID |
| ------- | ------- | -- |
| [[Services/System Block and Device Commands\|Device]] | Discover | 0x0000 |
|  | Ping | 0x0001 |
|  | Identify | 0x0002 |
|  | TimeSync | 0x0003 |
|  | Core discover | 0x0010 |
|  | SNDB | 0x0011-0x0013 |
|  | Bootloader passthrough | 0x0020-0x0021 |
| [[Services/Register\|Register]] | Enumerate blocks | 0x0100 |
|  | Enumerate fields | 0x0101 |
|  | Read | 0x0102 |
|  | Write | 0x0103 |
|  | Recall All | 0x0104 |
|  | Save All | 0x0105 |
|  | Create Dynamic | 0x0110 |
|  | Delete Dynamic | 0x0111 |
|  | Get Name | 0x0112 |
|  | Set Name | 0x0113 |
| [[Services/Log Handler\|Log Handler]] | Error reporter | 0x0200 |
|  | GetLogs | 0x0201 |
|  | ClearReadLogs | 0x0202 |
| [[Services/Storage\|Storage]] | Format Filesystem | 0x0300 |
|  | Create File | 0x0301 |
|  | Delete File | 0x0302 |
|  | Resize File | 0x0303 |
|  | Rename File | 0x0304 |
|  | Read File | 0x0305 |
|  | Write File | 0x0306 |
| [[Services/Subscriptions\|Subscriptions]] | ... | 0x0400-0x0421 |
| [[Services/Script\|Script]] | ... | 0x0500-0x0507 |
