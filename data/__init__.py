"""The co-op mod's data recipes, applied in this order by tools/build_pak.py to the player's own archives.

Each recipe module may define EDITS = [(member, [(anchor, replacement), ...]), ...], NEW_FILES and/or
build(ctx); see the docstring of tools/build_pak.py and docs/building.md. No game text is stored here
beyond the short anchors the edits need.
"""
RECIPES = [
    "coop_rules",     # co-op rules in network sessions: failed-autosave menu answered, AI replicated
    "coop_menu",      # the Co-op menu: main menu entry, host frame, the stock server set-up driven by it
    "coop_strings",   # the menu's texts in every language the game ships
]
