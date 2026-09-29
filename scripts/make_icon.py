from pathlib import Path
from PIL import Image, ImageDraw
root=Path(__file__).resolve().parent.parent
im=Image.new("RGBA",(256,256),(0,0,0,0))
d=ImageDraw.Draw(im)
d.rounded_rectangle((8,8,248,248),radius=48,fill=(24,24,24),outline=(100,100,100),width=3)
d.polygon([(68,44),(151,44),(188,82),(188,211),(68,211)],fill=(243,243,243))
d.polygon([(150,45),(150,83),(187,83)],fill=(133,133,133))
d.rounded_rectangle((90,107,160,125),radius=6,fill=(24,24,24))
d.rounded_rectangle((90,138,160,150),radius=5,fill=(100,100,100))
d.rounded_rectangle((90,164,138,176),radius=5,fill=(100,100,100))
im.save(root/'app/app.ico',sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])

