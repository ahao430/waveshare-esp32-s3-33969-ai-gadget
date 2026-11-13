import { defineStore } from 'pinia'

import { api, type ProviderInfo, type StoredSnapshot } from '@/api/client'

interface QuotaState {
  providers: ProviderInfo[]
  snapshots: Record<string, StoredSnapshot>
  loading: boolean
  error: string
}

export const useQuotaStore = defineStore('quota', {
  state: (): QuotaState => ({
    providers: [],
    snapshots: {},
    loading: false,
    error: '',
  }),
  actions: {
    async loadProviders() {
      this.loading = true
      this.error = ''
      try {
        const { providers } = await api.providers()
        this.providers = providers
      } catch (e) {
        this.error = e instanceof Error ? e.message : String(e)
      } finally {
        this.loading = false
      }
    },
    async loadSnapshot(id: string, force = false) {
      try {
        this.snapshots[id] = force ? await api.refresh(id) : await api.snapshot(id)
      } catch (e) {
        this.error = e instanceof Error ? e.message : String(e)
      }
    },
    async loadAll() {
      await this.loadProviders()
      await Promise.all(this.providers.map((p) => this.loadSnapshot(p.id)))
    },
    /** WS 推送到达时直接应用载荷（避免再打一次 HTTP） */
    applySnapshot(id: string, view: StoredSnapshot) {
      this.snapshots[id] = view
    },
  },
})
