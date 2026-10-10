# Run repeatedly by tools/statehash.sh (via execfile, with CFG = config path)
# inside the game. Once a game is loaded it hooks the BeginGameTurn event; when
# turn TARGET begins it writes a per-player state summary to OUT. Hooking the
# event (rather than polling) makes the snapshot independent of timing.
try:
    import CvEventInterface
    from CvPythonExtensions import CyGame, CyGlobalContext
    _em = CvEventInterface.getEventManager()
    if CyGame().getGameTurn() > 0 and not hasattr(_em, 'statehash_hooked'):
        _out, _target = open(CFG).read().split('\n')[:2]
        _target = int(_target)
        _orig = _em.EventHandlerMap['BeginGameTurn']
        def _hook(argsList, _orig=_orig, _out=_out, _target=_target):
            _orig(argsList)
            g = CyGame()
            if g.getGameTurn() != _target: return
            gc = CyGlobalContext()
            rows = ['turn %d' % g.getGameTurn()]
            for i in range(18):
                p = gc.getPlayer(i)
                if not p.isAlive(): continue
                rows.append('%d score %d gold %d units %d cities %d pop %d research %d power %d' % (
                    i, g.getPlayerScore(i), p.getGold(), p.getNumUnits(), p.getNumCities(),
                    p.getTotalPopulation(), p.getCurrentResearch(), p.getPower()))
            open(_out, 'w').write('\n'.join(rows) + '\n')
        _em.EventHandlerMap['BeginGameTurn'] = _hook
        _em.statehash_hooked = 1
        if CyGame().getAIAutoPlay() == 0: CyGame().setAIAutoPlay(1000)
except Exception:
    import traceback
    open(CFG + '.err', 'a').write(traceback.format_exc())
