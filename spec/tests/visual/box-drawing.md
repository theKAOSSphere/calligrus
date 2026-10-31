# Box drawing

The prose fonts have no box drawing, so it comes from Cascadia Code (the code font):

┌──────┬──────┐
│ Pi → │ CRC  │
└──────┴──────┘

In code the lines must meet and the columns stay on the grid, next to
characters beyond Latin-1 (arrows, ↔):

```
┌────────┬──────┬───────┐
│ Packet │ Dir  │ Check │
├────────┼──────┼───────┤
│ Cmd    │ Pi → │ sum   │
│ Hw     │ ↔    │ CRC   │
└────────┴──────┴───────┘
╔══╦══╗  ▁▂▃▄▅▆▇█  ░▒▓
╚══╩══╝  abcdefgh  xyz
```
