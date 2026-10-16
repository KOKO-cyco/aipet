import React from 'react';
import { View, Text, Switch, Input } from '@ray-js/ray';
import { NavBar, Slider, Icon } from '@ray-js/smart-ui';
import { useProps, useActions, useDevInfo } from '@ray-js/panel-sdk';
import { deviceSchema } from '@/devices/schema';
import { isOfflinePreview } from '@/devices';
import styles from './index.module.less';
import BitmapDp from './components/BitmapDp';
import RawDp from './components/RawDp';
import OtaCard from './components/OtaCard';
import Bolt from '@tuya-miniapp/icons/dist/svg/Bolt';
import SpeakerWaveLoud from '@tuya-miniapp/icons/dist/svg/SpeakerWaveLoud';
import Sun from '@tuya-miniapp/icons/dist/svg/Sun';
import Thermometer from '@tuya-miniapp/icons/dist/svg/Thermometer';
import Drop from '@tuya-miniapp/icons/dist/svg/Drop';
import Setting from '@tuya-miniapp/icons/dist/svg/Setting';
import Timer from '@tuya-miniapp/icons/dist/svg/Timer';
import Lock from '@tuya-miniapp/icons/dist/svg/Lock';
import Warning from '@tuya-miniapp/icons/dist/svg/Warning';

type SchemaItem = (typeof deviceSchema)[number];

const PREVIEW_MODE = isOfflinePreview();

function isWritable(dp: SchemaItem): boolean {
  return (dp as any).mode !== 'ro';
}

function isPrimarySwitch(dp: SchemaItem): boolean {
  const code = String((dp as any).code || '').toLowerCase();
  const t = ((dp as any).property || {}).type;
  return t === 'bool' && (code === 'switch' || code === 'switch_led' || /^switch_?$/.test(code) || code === 'power');
}

/* ─── Inline SVG icon set ───
   Each entry is a data:image/svg+xml URI imported from @tuya-miniapp/icons
   (1024 viewBox, fill=currentColor). smart-ui Icon's `isSvg` branch renders
   these via -webkit-mask; the visible color comes from the `color` prop. */
// Power symbol - @tuya-miniapp/icons has no power/switch icon, so use a
// custom data URI in the same format (1024 viewBox, fill=currentColor).
const POWER_ICON = 'data:image/svg+xml,' + encodeURIComponent(
  `<?xml version="1.0" standalone="no"?><!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd"><svg class="icon" width="200px" height="200.00px" viewBox="0 0 1024 1024" version="1.1" xmlns="http://www.w3.org/2000/svg"><path fill="currentColor" d="M448 128H576V512H448ZM240.5 240.5A384 384 0 1 0 783.5 240.5L693 331A256 256 0 1 1 331 331Z"/></svg>`
);
const ICON_PATHS: Record<string, string> = {
  power: POWER_ICON,
  battery: Bolt,
  charge: Bolt,
  volume: SpeakerWaveLoud,
  bright: Sun,
  temp: Thermometer,
  humid: Drop,
  mode: Setting,
  timer: Timer,
  lock: Lock,
  door: Warning,
  default: Warning,
};

function iconPathFor(code: string): string {
  const c = code.toLowerCase();
  if (/^switch/.test(c) || /power/.test(c)) return ICON_PATHS.power;
  if (/battery/.test(c)) return ICON_PATHS.battery;
  if (/charge/.test(c)) return ICON_PATHS.charge;
  if (/(volume|sound)/.test(c)) return ICON_PATHS.volume;
  if (/bright/.test(c) || /light/.test(c)) return ICON_PATHS.bright;
  if (/color_temp|colour_temp/.test(c)) return ICON_PATHS.temp;
  if (/temp/.test(c)) return ICON_PATHS.temp;
  if (/humid/.test(c)) return ICON_PATHS.humid;
  if (/(work_mode|^mode$)/.test(c)) return ICON_PATHS.mode;
  if (/(timer|countdown)/.test(c)) return ICON_PATHS.timer;
  if (/lock/.test(c)) return ICON_PATHS.lock;
  if (/(door|window)/.test(c)) return ICON_PATHS.door;
  return ICON_PATHS.default;
}

/* ───────────────────────── Device header ───────────────────────── */
function DeviceHeader({ name, isOnline }: { name: string; isOnline: boolean }) {
  return (
    <View className={styles.deviceCard}>
      <View className={styles.deviceIcon}>
        <Icon name={ICON_PATHS.power} size="40rpx" color="#ffffff" />
      </View>
      <View className={styles.deviceInfo}>
        <Text className={styles.deviceName}>{name}</Text>
        <View className={styles.deviceStatus}>
          <View
            className={`${styles.statusDot} ${PREVIEW_MODE ? styles.statusDotPreview : ''} ${
              !PREVIEW_MODE && !isOnline ? styles.statusDotOffline : ''
            }`}
          />
          <Text className={styles.deviceStatusText}>
            {PREVIEW_MODE ? 'Preview' : isOnline ? 'Online' : 'Offline'}
          </Text>
        </View>
      </View>
    </View>
  );
}

/* ──────── Primary switch — hero card with glow ──────── */
function HeroCard({ dp }: { dp: SchemaItem }) {
  const props = useProps((p: any) => p) as Record<string, any>;
  const actions = useActions() as Record<string, { set: (v: any) => Promise<boolean> }>;
  const devInfo = useDevInfo() as any;
  const value = !!props?.[dp.code];
  const readOnly = !isWritable(dp);
  const deviceName = PREVIEW_MODE ? 'Panel' : (devInfo?.name || 'Panel');
  const isOnline = PREVIEW_MODE ? true : !!devInfo?.isOnline;
  return (
    <View className={`${styles.hero} ${value ? styles.heroOn : styles.heroOff}`}>
      <View className={styles.heroHalo} />
      <View className={styles.heroHeader}>
        <View className={styles.heroBrand}>
          <Text className={styles.title}>{deviceName}</Text>
        </View>
        <View className={`${styles.statusPill} ${PREVIEW_MODE ? styles.statusPillPreview : ''}`}>
          <View className={`${styles.dot} ${!PREVIEW_MODE && !isOnline ? styles.dotOffline : ''}`} />
          <Text className={styles.statusText}>{PREVIEW_MODE ? 'Preview' : isOnline ? 'Online' : 'Offline'}</Text>
        </View>
      </View>
      <View className={styles.heroIcon}>
        <Icon name={ICON_PATHS.power} size="48rpx" color="#ffffff" />
      </View>
      <Text className={styles.heroName}>{(dp as any).name || dp.code}</Text>
      <Text className={styles.heroState}>{value ? 'ON' : 'OFF'}</Text>
      <View className={styles.heroToggle}>
        <Switch
          checked={value}
          disabled={readOnly}
          onChange={(e: any) => {
            if (readOnly) return;
            actions?.[dp.code]?.set?.(!!e?.detail?.value);
          }}
        />
      </View>
    </View>
  );
}

/* ───────────────────────── Section title ───────────────────────── */
function SectionTitle({ children }: { children: React.ReactNode }) {
  return (
    <View className={styles.sectionTitle}>
      <View className={styles.sectionAccent} />
      <Text className={styles.sectionTitleText}>{children}</Text>
    </View>
  );
}

/* ─────────────────────────── Bool row ──────────────────────────── */
function BoolRow({ dp }: { dp: SchemaItem }) {
  const props = useProps((p: any) => p) as Record<string, any>;
  const actions = useActions() as Record<string, { set: (v: any) => Promise<boolean> }>;
  const value = !!props?.[dp.code];
  const readOnly = !isWritable(dp);
  return (
    <View className={styles.card}>
      <View className={styles.cardRow}>
        <View className={`${styles.cardIcon} ${value ? styles.cardIconActive : ''}`}>
          <Icon name={iconPathFor((dp as any).code)} size="34rpx" color={value ? '#ffffff' : 'var(--app-M4, #ff7a45)'} />
        </View>
        <View className={styles.cardBody}>
          <Text className={styles.cardName}>{(dp as any).name || dp.code}</Text>
          <Text className={styles.cardMeta}>{readOnly ? 'read only' : (dp as any).code}</Text>
        </View>
        <View
          className={`${styles.boolSwitch} ${value ? styles.boolSwitchOn : ''} ${readOnly ? styles.boolSwitchDisabled : ''}`}
          onClick={() => {
            if (readOnly) return;
            actions?.[dp.code]?.set?.(!value);
          }}
        >
          <View className={`${styles.boolSwitchThumb} ${value ? styles.boolSwitchThumbOn : ''}`} />
        </View>
      </View>
    </View>
  );
}

/* ────────────────────────── Value row ──────────────────────────── */
function ValueRow({ dp }: { dp: SchemaItem }) {
  const props = useProps((p: any) => p) as Record<string, any>;
  const actions = useActions() as Record<string, { set: (v: any) => Promise<boolean> }>;
  const property: any = (dp as any).property || {};
  const min = typeof property.min === 'number' ? property.min : 0;
  const max = typeof property.max === 'number' ? property.max : 100;
  const step = typeof property.step === 'number' ? property.step : 1;
  const v = props?.[dp.code];
  const num = typeof v === 'number' ? v : min;
  const writable = isWritable(dp);
  const isBattery = /battery/.test(String(dp.code).toLowerCase());
  const unit = property.unit || (isBattery ? '%' : '');
  const accent = isBattery ? '#22c55e' : 'var(--app-M4, #ff7a45)';
  const pct = max > min ? Math.max(0, Math.min(100, Math.round(((num - min) / (max - min)) * 100))) : 0;

  const set = (val: number) => {
    const clamped = Math.max(min, Math.min(max, val));
    actions?.[dp.code]?.set?.(clamped);
  };

  // Readonly → stat tile with accent bar (battery shown green); writable → slider card
  if (!writable) {
    return (
      <View className={styles.card}>
        <View className={styles.cardRow}>
          <View className={styles.cardIcon}>
            <Icon name={iconPathFor((dp as any).code)} size="34rpx" color="var(--app-M4, #ff7a45)" />
          </View>
          <View className={styles.cardBody}>
            <Text className={styles.cardName}>{(dp as any).name || dp.code}</Text>
            <Text className={styles.cardMeta}>{(dp as any).code}</Text>
          </View>
        </View>
        <View className={styles.statBlock} style={{ color: accent } as any}>
          <View className={styles.statReadout}>
            <Text className={styles.statValue}>{num}</Text>
            {!!unit && <Text className={styles.statUnit}>{unit}</Text>}
          </View>
          <View className={styles.statBar}>
            <View className={styles.statBarFill} style={{ width: `${pct}%` } as any} />
          </View>
          <View className={styles.statRange}>
            <Text className={styles.statRangeLabel}>{min}{unit}</Text>
            <Text className={styles.statRangeLabel}>{max}{unit}</Text>
          </View>
        </View>
      </View>
    );
  }

  return (
    <View className={styles.card}>
      <View className={styles.sliderHeader}>
        <View className={styles.cardIcon}>
          <Icon name={iconPathFor((dp as any).code)} size="34rpx" color="var(--app-M4, #ff7a45)" />
        </View>
        <Text className={styles.sliderName}>{(dp as any).name || dp.code}</Text>
        <View className={styles.sliderValueWrap}>
          <Text className={styles.sliderValueBig}>{num}</Text>
          {!!unit && <Text className={styles.sliderValueUnit}>{unit}</Text>}
        </View>
      </View>
      <View className={styles.sliderWrap}>
        <Slider
          value={num}
          min={min}
          max={max}
          step={step}
          maxTrackHeight="8px"
          maxTrackColor="var(--app-B3, #e5e7eb)"
          minTrackHeight="8px"
          minTrackColor={accent}
          thumbWidth="26px"
          thumbHeight="26px"
          thumbColor="#ffffff"
          thumbBoxShadowStyle="0 4px 12px rgba(0,0,0,0.16)"
          onAfterChange={(val: number) => set(val)}
        />
      </View>
      <View className={styles.sliderRange}>
        <Text className={styles.sliderRangeLabel}>{min}{unit}</Text>
        <Text className={styles.sliderRangeLabel}>{max}{unit}</Text>
      </View>
    </View>
  );
}

/* ─────────────────────────── Enum row ──────────────────────────── */
function EnumRow({ dp }: { dp: SchemaItem }) {
  const props = useProps((p: any) => p) as Record<string, any>;
  const actions = useActions() as Record<string, { set: (v: any) => Promise<boolean> }>;
  const property: any = (dp as any).property || {};
  const range: string[] = Array.isArray(property.range) ? property.range : [];
  const value = props?.[dp.code];
  const readOnly = !isWritable(dp);
  return (
    <View className={styles.card}>
      <View className={`${styles.cardRow} ${styles.enumHeader}`}>
        <View className={styles.cardIcon}>
          <Icon name={iconPathFor((dp as any).code)} size="34rpx" color="var(--app-M4, #ff7a45)" />
        </View>
        <View className={styles.cardBody}>
          <Text className={styles.cardName}>{(dp as any).name || dp.code}</Text>
          <Text className={styles.cardMeta}>{readOnly ? 'read only' : (dp as any).code}</Text>
        </View>
      </View>
      <View className={styles.segmented}>
        {range.map((opt) => {
          const active = value === opt;
          return (
            <View
              key={opt}
              className={`${styles.segment} ${active ? styles.segmentActive : ''} ${
                readOnly ? styles.segmentDisabled : ''
              }`}
              onClick={() => {
                if (readOnly) return;
                actions?.[dp.code]?.set?.(opt);
              }}
            >
              <Text className={`${styles.segmentText} ${active ? styles.segmentTextActive : ''}`}>{opt}</Text>
            </View>
          );
        })}
      </View>
    </View>
  );
}

/* ────────────────────────── String row ─────────────────────────── */
function StringRow({ dp }: { dp: SchemaItem }) {
  const props = useProps((p: any) => p) as Record<string, any>;
  const actions = useActions() as Record<string, { set: (v: any) => Promise<boolean> }>;
  const value = props?.[dp.code];
  const valueStr = value === undefined || value === null ? '' : String(value);
  const readOnly = !isWritable(dp);
  const property: any = (dp as any).property || {};
  const maxlen = typeof property.maxlen === 'number' ? property.maxlen : 0;
  const [draft, setDraft] = React.useState('');
  const [sentAt, setSentAt] = React.useState(0);
  const [error, setError] = React.useState('');
  React.useEffect(() => {
    setDraft(valueStr);
  }, [valueStr]);
  const justSent = sentAt > 0 && Date.now() - sentAt < 1500;

  const handleSend = () => {
    if (readOnly) return;
    if (maxlen > 0 && draft.length > maxlen) {
      setError(`Too long (max ${maxlen})`);
      return;
    }
    setError('');
    actions?.[dp.code]
      ?.set?.(draft)
      .then(() => setSentAt(Date.now()))
      .catch((err: any) => setError(String(err?.message ?? 'Send failed')));
  };

  return (
    <View className={styles.card}>
      <View className={styles.cardRow}>
        <View className={styles.cardIcon}>
          <Icon name={iconPathFor((dp as any).code)} size="34rpx" color="var(--app-M4, #ff7a45)" />
        </View>
        <View className={styles.cardBody}>
          <Text className={styles.cardName}>{(dp as any).name || dp.code}</Text>
          {maxlen > 0 && <Text className={styles.cardMeta}>{draft.length}/{maxlen}</Text>}
        </View>
      </View>
      {readOnly ? (
        <Text className={styles.stringReadonly}>{valueStr || '-'}</Text>
      ) : (
        <View className={styles.stringInputRow}>
          <Input
            className={styles.stringInput}
            value={draft}
            placeholder="Enter text…"
            maxlength={maxlen > 0 ? maxlen : -1}
            onInput={(e: any) => {
              setDraft(e.detail?.value ?? '');
              if (error) setError('');
            }}
          />
          <View
            className={`${styles.stringSendBtn} ${justSent ? styles.stringSendBtnSuccess : ''}`}
            onClick={handleSend}
          >
            <Text className={styles.stringSendBtnText}>{justSent ? '✓' : '↗'}</Text>
          </View>
        </View>
      )}
      {!!error && <Text className={styles.stringError}>{error}</Text>}
    </View>
  );
}

/* ──────────────────────────── Dispatch ─────────────────────────── */
function DpRow({ dp }: { dp: SchemaItem }) {
  const t = ((dp as any).property || {}).type;
  if (t === 'bool') return <BoolRow dp={dp} />;
  if (t === 'value') return <ValueRow dp={dp} />;
  if (t === 'enum') return <EnumRow dp={dp} />;
  if (t === 'bitmap') return <BitmapDp dp={dp as any} />;
  if (t === 'raw') return <RawDp dp={dp as any} />;
  return <StringRow dp={dp} />;
}

/* ───────────────────────────── Home ────────────────────────────── */
export default function Home() {
  const devInfo = useDevInfo() as any;
  const deviceName = PREVIEW_MODE ? 'Device' : devInfo?.name || 'Device';
  const isOnline = PREVIEW_MODE ? true : !!devInfo?.isOnline;
  const list = deviceSchema as readonly SchemaItem[];
  const primary = list.find(isPrimarySwitch);
  const rest = primary ? list.filter((d) => d !== primary) : list;
  const writable = rest.filter(isWritable);
  const readonly = rest.filter((d) => !isWritable(d));
  const [isUpgrading, setIsUpgrading] = React.useState(false);

  return (
    <View>
      <NavBar leftText={deviceName} leftTextType="home" border safeAreaInsetTop background="var(--app-B1, #ffffff)" />
      <View className={styles.container}>
        {primary ? (
          <HeroCard dp={primary} />
        ) : (
          <DeviceHeader name={deviceName} isOnline={isOnline} />
        )}

        {list.length === 0 ? (
          <View className={styles.emptyCard}>
            <View className={styles.emptyIconWrap}>
              <Icon name={ICON_PATHS.default} size="72rpx" color="var(--app-M4, #ff7a45)" />
            </View>
            <Text className={styles.emptyTitle}>No DPs synced yet</Text>
            <Text className={styles.emptyHint}>
              Run "Sync DPs from Cloud" in the TuyaOpen IDE MiniApp panel to generate
              src/devices/schema.ts.
            </Text>
          </View>
        ) : (
          <View
            style={{ pointerEvents: isUpgrading ? 'none' : undefined, opacity: isUpgrading ? 0.5 : 1 } as any}
          >
            {writable.length > 0 && (
              <View className={styles.section}>
                <SectionTitle>Control</SectionTitle>
                {writable.map((dp) => (
                  <DpRow key={dp.code} dp={dp} />
                ))}
              </View>
            )}
            {readonly.length > 0 && (
              <View className={styles.section}>
                <SectionTitle>Status</SectionTitle>
                {readonly.map((dp) => (
                  <DpRow key={dp.code} dp={dp} />
                ))}
              </View>
            )}
          </View>
        )}

        <OtaCard onUpgradingChange={setIsUpgrading} />
      </View>
    </View>
  );
}
