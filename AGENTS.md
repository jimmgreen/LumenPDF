# LumenPDF
Independent Windows C++20 application. LUMENUI is a read-only sibling dependency.
Keep PDF behavior in core/, application interaction in app/. Never move it into LUMEN.
Use MuPDF 1.28.4 via scripts/bootstrap.ps1 (hash-pinned, no JS or OCR).
Run build.bat and the pdf_core test. Report untested Word/LibreOffice, IME and mouse interactions honestly.
All engine access is serialized off the UI thread. Never overwrite a source on failed conversion/save.

