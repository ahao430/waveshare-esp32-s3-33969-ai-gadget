/* icon_embed — 自动生成，勿手改。按文件名（无扩展名）查嵌入图标。 */
#ifndef ICON_EMBED_H
#define ICON_EMBED_H

#include "lvgl.h"

/* key=图标文件名（如 "sub"）；未嵌入返回 NULL（调用方回退占位）。 */
const lv_image_dsc_t *ui_icon_find(const char *key);

#endif /* ICON_EMBED_H */
