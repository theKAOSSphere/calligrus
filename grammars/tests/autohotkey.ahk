#Requires AutoHotkey v2.0
#SingleInstance Force

; Toggle a window's always-on-top state
^!t:: {
    hwnd := WinExist("A")
    WinSetAlwaysOnTop(-1, hwnd)
    ToolTip("Toggled " A_ScriptName)
    SetTimer(() => ToolTip(), -1500)
}

class Counter {
    count := 0
    Increment(by := 1) {
        this.count += by
        return this.count
    }
}

::btw::by the way
c := Counter()
MsgBox("Count: " c.Increment(2) ", ok " true, "Title", 0x40)
