import os
names = ["bahnschrift.ttf", "bahnschrift-355.ttf", "bahnschrift-750.ttf", "arialnb.ttf",
         "arialn.ttf", "verdanab.ttf", "ariblk.ttf", "seguiemj.ttf", "tahoma.ttf",
         "consolab.ttf", "mstyc.ttf", "segoeui.ttf", "segoeuib.ttf", "segoeuib2.ttf"]
for n in names:
    print(n, os.path.exists(r"C:\Windows\Fonts\\" + n))
