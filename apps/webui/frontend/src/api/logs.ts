import { api } from './client'
import type {
  LogFile, LogContent, LogStats, LogAggregate,
  TimelineBucket, SetLevelResult, FlushResult,
} from '@/types/api'

export const logsApi = {
  files: (params?: { logger?: string; date?: string; limit?: number; offset?: number }) => {
    const query = new URLSearchParams()
    if (params?.logger) query.set('logger', params.logger)
    if (params?.date) query.set('date', params.date)
    query.set('limit', String(params?.limit ?? 30))
    query.set('offset', String(params?.offset ?? 0))
    return api.get<LogFile[]>(`/api/logs/files?${query.toString()}`)
  },

  content: (params: {
    file: string
    offset?: number
    limit?: number
    level?: string
    keyword?: string
    from?: string
    to?: string
    from_end?: boolean
  }) => {
    const query = new URLSearchParams()
    query.set('file', params.file)
    query.set('offset', String(params.offset ?? 0))
    query.set('limit', String(params.limit ?? 500))
    if (params.level) query.set('level', params.level)
    if (params.keyword) query.set('keyword', params.keyword)
    if (params.from) query.set('from', params.from)
    if (params.to) query.set('to', params.to)
    if (params.from_end) query.set('from_end', 'true')
    return api.get<LogContent>(`/api/logs/content?${query.toString()}`)
  },

  stats: (params: { file: string; from?: string; to?: string; logger?: string }) => {
    const query = new URLSearchParams()
    query.set('file', params.file)
    if (params.from) query.set('from', params.from)
    if (params.to) query.set('to', params.to)
    if (params.logger) query.set('logger', params.logger)
    return api.get<LogStats>(`/api/logs/stats?${query.toString()}`)
  },

  aggregate: (params: { file: string; level?: string; limit?: number }) => {
    const query = new URLSearchParams()
    query.set('file', params.file)
    query.set('level', params.level ?? 'error')
    query.set('limit', String(params.limit ?? 20))
    return api.get<LogAggregate[]>(`/api/logs/aggregate?${query.toString()}`)
  },

  timeline: (params: { file: string; bucket?: string }) => {
    const query = new URLSearchParams()
    query.set('file', params.file)
    query.set('bucket', params.bucket ?? 'minute')
    return api.get<TimelineBucket[]>(`/api/logs/timeline?${query.toString()}`)
  },

  setLevel: (targets: string[], level: string) =>
    api.post<{ results: SetLevelResult[] }>('/api/logs/level', { targets, level }),

  flush: (targets: string[]) =>
    api.post<{ results: FlushResult[] }>('/api/logs/flush', { targets }),
}
