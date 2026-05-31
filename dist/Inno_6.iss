[Setup]
AppName=TornadoFanController
AppVersion=0.5
DefaultDirName={pf}\TornadoFanController
DefaultGroupName=TornadoFanController
UninstallDisplayIcon={app}\mygui.exe
OutputDir=.
OutputBaseFilename=TornadoFanControllerInstaller
Compression=lzma
SolidCompression=yes

[Files]
Source: "..\.\CPP\FanControl\x64\Release\FanControl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "inpoutx64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "fanctrl_settings.dat"; DestDir: "{app}"; Flags: ignoreversion
Source: "ui.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{commondesktop}\TornadoFanController"; Filename: "{app}\ui.exe"; WorkingDir: "{app}"

[Registry]
; Add backend to autostart
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "MyFanControllerService"; ValueData: """{app}\FanControl.exe"""