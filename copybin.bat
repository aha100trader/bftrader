rem bin
del .\bftrader\datafeed.exe
copy .\datafeed\release\datafeed.exe .\bftrader\datafeed.exe

del .\bftrader\ctpgateway.exe
copy .\ctpgateway\release\ctpgateway.exe .\bftrader\ctpgateway.exe

del .\bftrader\qtdemo.exe
copy .\tools\qtdemo\release\qtdemo.exe .\bftrader\qtdemo.exe

del .\bftrader\bugreport.exe
copy .\tools\bugreport\release\bugreport.exe .\bftrader\bugreport.exe

rem pdb
del .\bftrader\datafeed.pdb
copy .\datafeed\release\datafeed.pdb .\bftrader\datafeed.pdb

del .\bftrader\ctpgateway.pdb
copy .\ctpgateway\release\ctpgateway.pdb .\bftrader\ctpgateway.pdb

del .\bftrader\qtdemo.pdb
copy .\tools\qtdemo\release\qtdemo.pdb .\bftrader\qtdemo.pdb

del .\bftrader\bugreport.pdb
copy .\tools\bugreport\release\bugreport.pdb .\bftrader\bugreport.pdb