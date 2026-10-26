/**
 * 由 scripts/gen_payloads.py 生成，勿手改。
 *
 * 这些是 read() 返回的原始载荷（`diag` 与实现内部用）。对外产品面用的是
 * editor.d.ts 里的 Pos / Caret / Selection 这类形状，由实现从这里映射过去。
 *
 * optional 的键在类型上是可选的（`?`）：它们在 C++ 里写在 if 里，不保证出现。
 */

export interface CaretStyle {
  color: unknown[];
  widthPt: number;
}

export interface SelectionStyle {
  color: unknown[];
}

export interface Stats {
  pageCount: number;
  glyphCount: number;
  placedLines: number;
  clusters: number;
  faces: number;
  fontInstances: number;
  fontRunCount: number;
  missingGlyphCount: number;
  fontCacheHits: number;
  fontCacheMisses: number;
  fontStorageReads: number;
  fontStorageBytes: number;
  fontIoMs: number;
  fontResidentBytes: number;
  fontStorageFailures: number;
  fontStorageEofs: number;
  shapeMs: number;
  layoutMs: number;
  pageFpMs: number;
  paintMs: number;
  paintFillMs: number;
  paintGlyphMs: number;
  paintGlyphBoundsMs: number;
  paintedGlyphs: number;
  paintPresentMs: number;
  paintPreMs: number;
  paintSwapMs: number;
  paintTransientMs: number;
  paintOverlayMs: number;
  paintLedgerMs: number;
  paintReportMs: number;
  totalMs: number;
  frameWidth: number;
  frameHeight: number;
  frameBytes: number;
  glyphCacheBytes: number;
  glyphCacheLimit: number;
  zoom: number;
}

export interface BenchRelayoutPaint {
  runs: number;
  shapeMs: number;
  layoutMs: number;
  paintMs: number;
  totalMs: number;
}

export interface FlowTextPos {
  para: number;
  offset: number;
}

export interface SelectionReport {
  anchor: Record<string, unknown>;
  focus: Record<string, unknown>;
  empty: boolean;
  rects: unknown[];
}

export interface HitTest {
  para: number;
  offset: number;
  field: number;
  side: number;
  line: number;
  opt: number;
  cellPara?: number;
  cell?: unknown[];
  img?: number;
  imgHandle?: number;
}

export interface ImageList {
  count: number;
  bytes: number;
  images: unknown[];
}

export interface ImageHit {
  index: number;
  handle: number;
}

export interface Controls {
  mode: number;
  count: number;
  caret: number;
  items: unknown[];
}

export interface Lines {
  lines: unknown[];
  glyphs: unknown[];
}

export interface BracketSlots {
  slots: unknown[];
}

export interface EditStats {
  layoutRan: boolean;
  relaidLines: number;
  totalLines: number;
  relaidParagraphs: number;
  reusedParagraphs: number;
  suffixShifted: boolean;
  totalParagraphs: number;
  layoutWasFull: boolean;
  dirtyPageCount: number;
  repaintedPages: number;
  totalPages: number;
  windowPages: number;
  windowFirstPage: number;
  bandY0: number;
  bandY1: number;
  repaintedPixels: number;
  repaintedX: number;
  repaintedY: number;
  repaintedW: number;
  repaintedH: number;
  frameW: number;
  frameH: number;
  framePixels: number;
  layoutMs: number;
  rebuildCompileMs: number;
  rebuildReused: number;
  rebuildFlowMs: number;
  rebuildFlowPosMs: number;
  rebuildCopyMs: number;
  rebuildControlsMs: number;
  rebuildPath: number;
  rebuildReject: number;
  pageFpMs: number;
  paintMs: number;
  totalMs: number;
  caretPara: number;
  caretOffset: number;
  glyphCount: number;
  missingGlyphCount: number;
  fontCacheHits: number;
  fontCacheMisses: number;
  fontStorageReads: number;
  fontStorageBytes: number;
  fontIoMs: number;
  fontResidentBytes: number;
  fontStorageFailures: number;
  fontStorageEofs: number;
  caretIndex: number;
  docCodepoints: number;
  caretSide: number;
  caretVertical: boolean;
  caretBlockReversed: boolean;
  caretPage: number;
  caretVisible: boolean;
  caretX: number;
  caretY: number;
  caretW: number;
  caretH: number;
  editMode: number;
  controlCount: number;
  caretControl: number;
  blockedEdits: number;
}

export interface Diff {
  diff: number;
  frameW: number;
  frameH: number;
  box?: number;
}

export interface AuditPage {
  diff: number;
  w: number;
  h: number;
  page: number;
  cached: number;
  box?: number;
}

export interface AuditTables {
  diff: number;
  cells: number;
  emptyCell: number;
  badRange: number;
  overlap: number;
  noMark: number;
  pathMismatch: number;
  badSpan: number;
  stray: number;
  firstCell: number;
}

export interface AuditLayout {
  note: number;
  diff: number;
  glyphDiff: number;
  lineDiff: number;
  paraDiff: number;
  startsDiff: number;
  tableDiff: number;
  statsSame: boolean;
  first: Record<string, unknown>;
  glyphs: number;
  lines: number;
  paras: number;
  relaidParagraphs: number;
  relaidLines: number;
  layoutWasFull: boolean;
  curMs: number;
  fullMs: number;
}

export interface AuditGlyphPages {
  diff: number;
  pages: number;
  glyphs: number;
  badPages: number;
  firstBadPage: number;
  missing: number;
  extra: number;
  stray: number;
}

export interface AuditCompile {
  note: number;
  diff: number;
  paras: number;
  curMs: number;
  fullMs: number;
}

export interface TableState {
  inTable: number;
  rows: number;
  cols: number;
  widthPt: number;
  align: number;
  headerRows: number;
  row: number;
  rowHeader: number;
  rowSplit: number;
  rowBreak: number;
  heightPt: number;
  heightRule: number;
  cellRow: number;
  cellCol: number;
  rowSpan: number;
  colSpan: number;
  vAlign: number;
  selCells: number;
  selRows: number;
  selCols: number;
}

export interface ContextTarget {
  kind: number;
  para: number;
  offset: number;
  cellTable: number;
  cellRow: number;
  cellCol: number;
  control: number;
  image: number;
}

/** 载荷名 -> 形状。readPayload() 用它做运行时校验。 */
export interface Payloads {
  caretStyle: CaretStyle;
  selectionStyle: SelectionStyle;
  stats: Stats;
  benchRelayoutPaint: BenchRelayoutPaint;
  flowTextPos: FlowTextPos;
  selectionReport: SelectionReport;
  hitTest: HitTest;
  imageList: ImageList;
  imageHit: ImageHit;
  controls: Controls;
  lines: Lines;
  bracketSlots: BracketSlots;
  editStats: EditStats;
  diff: Diff;
  auditPage: AuditPage;
  auditTables: AuditTables;
  auditLayout: AuditLayout;
  auditGlyphPages: AuditGlyphPages;
  auditCompile: AuditCompile;
  tableState: TableState;
  contextTarget: ContextTarget;
}

export type PayloadName = keyof Payloads;
