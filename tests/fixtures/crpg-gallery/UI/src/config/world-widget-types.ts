import type { ComponentType } from 'react';
import type { WorldWidgetEntry } from '@/stores/world-widget-store';

/**
 * Registry of world-space widget types(老 WorldWidget 系统)。`type` 字段对应 C++
 * `WorldWidgetAnchorComponent::WebWidgetType`,通过这个表分发到 React 组件。
 *
 * Keep keys kebab-case so they round-trip cleanly through FName.
 *
 * **HP 条已迁移到新 FloatingUI 系统**(`floating-ui-types.ts` 里的 `'health-bar'` entry),
 * 老 WorldWidget 暂时没有内置 widget 类型,此表保留是为了将来如果有 widget 想用"自带
 * 距离-缩放 + occlusion check + 一身配齐"的老 anchor 模型 时可以注册。新 floating-ui
 * 系统更轻量(anchor 只管位置,VM/type/visual 配置都在 instance 上),新需求优先走新系统。
 */
export type WorldWidgetComponent = ComponentType<{ widget: WorldWidgetEntry }>;

export const WORLD_WIDGET_TYPES: Record<string, WorldWidgetComponent> = {};
