"""Build a simple editable Forge Grid scene from textures supplied in this folder.
Blender 3.6-4.5+, run in the Scripting workspace or background.
"""
import bpy
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SIZE = 32.0  # 32 Blender units, from -16 to +16
NAME = "forge_grid_surface"

for ob in list(bpy.data.objects):
    bpy.data.objects.remove(ob, do_unlink=True)

mesh = bpy.data.meshes.new(NAME)
s = SIZE / 2.0
mesh.from_pydata([(-s,-s,0),(s,-s,0),(s,s,0),(-s,s,0)], [], [(0,1,2,3)])
mesh.update()
uv = mesh.uv_layers.new(name="UVMap")
for loop in mesh.loops:
    co = mesh.vertices[loop.vertex_index].co
    uv.data[loop.index].uv = ((co.x+s)/SIZE, (co.y+s)/SIZE)
obj = bpy.data.objects.new(NAME,mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active=obj
obj.select_set(True)
obj["halo_asset_id"] = "forge_grid"
obj["collision"] = "none"

mat = bpy.data.materials.new("Forge Grid Hologram")
mat.use_nodes = True
mat.use_backface_culling = False
if hasattr(mat, "surface_render_method"):
    try: mat.surface_render_method = 'BLENDED'
    except (TypeError, ValueError): pass
elif hasattr(mat, "blend_method"):
    mat.blend_method = 'BLEND'

ns=mat.node_tree.nodes
ns.clear()
ln=mat.node_tree.links
out=ns.new('ShaderNodeOutputMaterial')
out.location=(500,0)
bsdf=ns.new('ShaderNodeBsdfPrincipled')
bsdf.location=(200,0)
bsdf.inputs['Metallic'].default_value=0.0
bsdf.inputs['Roughness'].default_value=1.0
col=ns.new('ShaderNodeTexImage')
col.location=(-300,180)
col.image=bpy.data.images.load(str(ROOT/'forge_grid_color.png'), check_existing=True)
emi=ns.new('ShaderNodeTexImage')
emi.location=(-300,-200)
emi.image=bpy.data.images.load(str(ROOT/'forge_grid_emissive.png'), check_existing=True)
ln.new(col.outputs['Color'],bsdf.inputs['Base Color'])
ln.new(col.outputs['Alpha'],bsdf.inputs['Alpha'])
if 'Emission Color' in bsdf.inputs:
    ln.new(emi.outputs['Color'],bsdf.inputs['Emission Color'])
elif 'Emission' in bsdf.inputs:
    ln.new(emi.outputs['Color'],bsdf.inputs['Emission'])
if 'Emission Strength' in bsdf.inputs:
    bsdf.inputs['Emission Strength'].default_value=1.0
ln.new(bsdf.outputs['BSDF'],out.inputs['Surface'])
obj.data.materials.append(mat)

# Embed images in the blend so the editable file stays portable.
col.image.pack()
emi.image.pack()
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'forge_grid.blend'))
bpy.ops.export_scene.gltf(filepath=str(ROOT/'forge_grid.glb'),export_format='GLB',
    use_selection=True,export_materials='EXPORT',export_texcoords=True,
    export_normals=True,export_extras=True)
print('Forge Grid created:',ROOT/'forge_grid.glb')
