Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
shell.Run "pythonw.exe """ & fso.BuildPath(fso.GetParentFolderName(WScript.ScriptFullName), "palette_watcher.py") & """", 0, False
