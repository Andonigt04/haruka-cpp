"""Genera Tests/Data/prefab_blender.glb CON BLENDER, para que el test pruebe lo que Blender exporta de
verdad (conversión Z-arriba -> Y-arriba, nombres de malla, propiedades personalizadas) y no lo que uno
cree que exporta.

    blender -b --factory-startup -P Tests/Data/gen_prefab_blender.py

Lo que monta (en ejes de BLENDER; el test lo comprueba ya convertido a glTF: (x, y, z) -> (x, z, -y)):
  b0  'brick' enlazado en el origen
  b1  'brick' enlazado en (1, 2, 3), girado 90 grados sobre Z
  muro  vacío en (10, 0, 0) girado 90 grados sobre Z, con b2 ('brick' enlazado) de hijo en local (1, 0, 0)
  b3  copia NO enlazada: su malla se llama 'brick.001' y tiene que contar como 'brick'
  b4  'brick' con propiedades item='hull_beam', joint='mechanism', arc=12.5
  b5  'brick' ESCALADO x2: tiene que descartarse
  anclaje_1  vacío en (0, 0, 5)
"""
import math
import os
import bpy

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prefab_blender.glb")

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

# La caja del item, centrada en su origen. 'brick' mide 0.24 x 0.11 x 0.07 en ejes del MOTOR (Y arriba);
# en Blender (Z arriba) eso es 0.24 x 0.07 x 0.11.
sx, sy, sz = 0.24, 0.07, 0.11
v = [(x * sx / 2, y * sy / 2, z * sz / 2) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
f = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
brick = bpy.data.meshes.new("brick")
brick.from_pydata(v, [], f)

def obj(name, mesh, loc, rot_z_deg=0.0, parent=None):
    o = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(o)
    o.location = loc
    o.rotation_euler = (0.0, 0.0, math.radians(rot_z_deg))
    if parent:
        o.parent = parent   # matrix_parent_inverse queda en identidad: mundo = padre * local
    return o

obj("b0", brick, (0, 0, 0))
obj("b1", brick, (1, 2, 3), 90)
muro = obj("muro", None, (10, 0, 0), 90)
obj("b2", brick, (1, 0, 0), parent=muro)
obj("b3", brick.copy(), (0, 0, 1))            # Blender la llama 'brick.001'
b4 = obj("b4", brick, (0, 0, 2))
b4["item"] = "hull_beam"
b4["joint"] = "mechanism"
b4["arc"] = 12.5
b5 = obj("b5", brick, (0, 0, 3))
b5.scale = (2, 2, 2)
obj("anclaje_1", None, (0, 0, 5))

bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', export_extras=True, export_yup=True)
print("escrito", OUT, os.path.getsize(OUT), "bytes")
