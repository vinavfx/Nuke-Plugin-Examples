import os
import nuke  # type: ignore

build = os.path.join(os.path.dirname(__file__), 'build')
nuke.pluginAddPath(build)

menu = nuke.menu('Nodes').addMenu('Examples')

for p in os.listdir(build):
    name = p.split('.')[0]
    nuke.load(name)
    menu.addCommand(name)
