import os
import nuke  # type: ignore

build = os.path.join(os.path.dirname(__file__), 'build')
plugins = '{}/Nuke{}.{}'.format(build,
                                nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR)

if os.path.isdir(plugins):
    nuke.pluginAddPath(plugins)
    menu = nuke.menu('Nodes').addMenu('Examples')

    for p in os.listdir(plugins):
        name = p.split('.')[0]
        nuke.load(name)
        menu.addCommand(name)
