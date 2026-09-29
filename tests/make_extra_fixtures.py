from pathlib import Path
from pypdf import PdfWriter
from pypdf.generic import DictionaryObject, NameObject as N, NumberObject as I, ArrayObject as A, TextStringObject as T, DecodedStreamObject, BooleanObject
out=Path(__file__).parent/'fixtures'
w=PdfWriter()
p=w.add_blank_page(400,600)
font=w._add_object(DictionaryObject({N('/Type'):N('/Font'),N('/Subtype'):N('/Type1'),N('/BaseFont'):N('/Helvetica')}))
res=DictionaryObject({N('/Font'):DictionaryObject({N('/F1'):font})})
p[N('/Resources')]=res
stream=DecodedStreamObject();stream.set_data(b'BT /F1 16 Tf 40 540 Td (Original vector text) Tj ET')
p[N('/Contents')]=w._add_object(stream)
ap=DecodedStreamObject();ap.set_data(b'0.9 g 0 0 220 35 re f 0 g BT /F1 12 Tf 8 12 Td (FORM VALUE 42) Tj ET')
ap.update({N('/Type'):N('/XObject'),N('/Subtype'):N('/Form'),N('/BBox'):A([I(0),I(0),I(220),I(35)]),N('/Resources'):res})
widget=DictionaryObject({N('/Type'):N('/Annot'),N('/Subtype'):N('/Widget'),N('/FT'):N('/Tx'),N('/T'):T('answer'),N('/V'):T('FORM VALUE 42'),N('/Rect'):A([I(40),I(440),I(260),I(475)]),N('/F'):I(4),N('/AP'):DictionaryObject({N('/N'):w._add_object(ap)}),N('/DA'):T('/F1 12 Tf 0 g')})
ref=w._add_object(widget);p[N('/Annots')]=A([ref])
w._root_object[N('/AcroForm')]=w._add_object(DictionaryObject({N('/Fields'):A([ref]),N('/NeedAppearances'):BooleanObject(False),N('/DR'):res,N('/DA'):T('/F1 12 Tf 0 g')}))
p2=w.add_blank_page(700,400);p2[N('/Rotate')]=I(90);p2[N('/CropBox')]=A([I(30),I(20),I(660),I(380)])
with (out/'forms-mixed.pdf').open('wb') as f:w.write(f)
w.encrypt('reader','owner',use_128bit=True)
with (out/'password.pdf').open('wb') as f:w.write(f)

from pypdf import PdfReader
hidden=PdfWriter(clone_from=out/'forms-mixed.pdf')
hidden.pages[0]['/Annots'][0].get_object()[N('/F')]=I(32)
with (out/'no-view.pdf').open('wb') as f:hidden.write(f)
