<script setup lang="ts">
// 小智设置（2026-11-15 新增独立 tab）。
// 设备端「小智」App 走 Bridge 语音链路（ASR → 回复 → TTS），音色在
// 「语音设置」按 App 配置；小智角色/大模型在官方控制台配置——这里提供入口。
import { RouterLink } from 'vue-router'
</script>

<template>
  <h1 class="page-title">小智</h1>
  <p class="page-sub">
    设备端「小智」App：按住说话 → Bridge 转写 → 回复 → 本地 TTS 朗读。
    识别 Key 与朗读音色在这里的下方入口配置；AI 角色、唤醒词、绑定设备在小智官方后台。
  </p>

  <div class="card">
    <h3 class="mb-2 font-semibold">小智后台</h3>
    <p class="mb-3 text-sm text-dim">
      设备角色（大模型/音色/人设）、设备绑定与激活序列号都在小智官方控制台管理：
      首次配网后设备屏幕会显示激活序列号，在控制台「设备管理」里绑定即可切换角色。
    </p>
    <div class="flex flex-wrap items-center gap-3">
      <a class="btn" href="https://xiaozhi.me/console" target="_blank" rel="noopener">
        打开小智后台 ↗
      </a>
      <a class="btn-ghost" href="https://xiaozhi.me" target="_blank" rel="noopener">
        官网 xiaozhi.me ↗
      </a>
      <a class="btn-ghost" href="https://github.com/78/xiaozhi-esp32" target="_blank" rel="noopener">
        GitHub 项目 ↗
      </a>
    </div>
  </div>

  <div class="card mt-4">
    <h3 class="mb-2 font-semibold">本机相关设置</h3>
    <div class="flex flex-wrap items-center gap-3 text-sm">
      <RouterLink to="/voice" class="badge !bg-accent/15 !text-accent hover:!bg-accent/25">
        去语音设置（小智 App 音色 / ASR Key）→
      </RouterLink>
      <RouterLink to="/network" class="badge hover:!bg-cardhi">
        去网络与配网（Wi-Fi / 首次配网）→
      </RouterLink>
    </div>
    <p class="mt-3 text-xs text-dim">
      小智 App 的对话回复默认走 AstrBot/常规链路；后续接入小智官方服务端
      （WebSocket 协议）时，连接配置会加在本页。
    </p>
  </div>
</template>
