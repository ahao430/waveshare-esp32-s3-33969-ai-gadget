/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** Bridge 地址，默认走 Vite 开发代理 */
  readonly VITE_BRIDGE_URL?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
