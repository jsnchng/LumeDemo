{
    "compatibility_info": {
        "version": "22.00",
        "type": "shader"
    },
    "category": "Render/Effect",
    "displayName": "Split Screen Display",
    "vert": "rendershaders://shader/fullscreen_triangle.vert.spv",
    "frag": "assets://app/shader/split_screen_display.frag.spv",
    "state": {
        "colorBlendState": {
            "colorAttachments": [
                {
                    "colorWriteMask": "r_bit|g_bit|b_bit|a_bit"
                }
            ]
        }
    }
}