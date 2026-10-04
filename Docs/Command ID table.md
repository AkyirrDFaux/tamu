| Section       | Command                | Enum (16bit Hex) |
| ------------- | ---------------------- | ---------------- |
| Device        | Discover               | 0x0000           |
|               | Ping                   | 0x0001           |
|               | Identify               | 0x0002           |
|               | TimeSync               | 0x0003           |
|               | Core discover          | 0x0010           |
|               | SNDB                   | 0x0011-0x0013    |
|               | Bootloader passthrough | 0x0020-0x0021    |
| Registry      | Enumerate Block        | 0x0100           |
|               | Enumerate Field        | 0x0101           |
|               | Read                   | 0x0102           |
|               | Write                  | 0x0103           |
|               | Recall All             | 0x0104           |
|               | Save All               | 0x0105           |
|               | Create Dynamic         | 0x0110           |
|               | Delete Dynamic         | 0x0111           |
|               | Get Name               | 0x0112           |
|               | Set Name               | 0x0113           |
| Log Handler   | Report Error           | 0x0200           |
|               | Read Logs              | 0x0201           |
|               | Clear Logs             | 0x0202           |
| Storage       | Format                 | 0x0300           |
|               | Create                 | 0x0301           |
|               | Delete                 | 0x0302           |
|               | Resize                 | 0x0303           |
|               | Rename                 | 0x0304           |
|               | Read                   | 0x0305           |
|               | Write                  | 0x0306           |
| Subscriptions | ...                    | 0x0400-0x0421    |
| Script        | ...                    | 0x0500-0x0507    |


