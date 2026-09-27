block {
    name = "recovered",
    update = function(ctx)
        ctx:set { full_text = "recovered" }
    end,
}

block {
    name = "middle",
    update = function(ctx)
        ctx:set { full_text = "middle" }
    end,
}

block {
    name = "bottom",
    update = function(ctx)
        ctx:set { full_text = "bottom" }
    end,
}
