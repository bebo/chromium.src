@ECHO ON
set TEMPDIR=.\out\nw\dist\temp_pdb
set DESTDIR=.\out\nw\dist

rmdir /S /Q %TEMPDIR%
mkdir %TEMPDIR%

xcopy /Y .\out\nw\*.pdb %TEMPDIR%
xcopy /Y .\out\nw\*.dll %TEMPDIR%
xcopy /Y .\out\nw\*.exe %TEMPDIR%

python -m zipfile -c %DESTDIR%\pdb_symbols.zip %TEMPDIR%

rmdir /S /Q %TEMPDIR%
