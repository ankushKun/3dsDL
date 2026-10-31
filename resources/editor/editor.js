// The code editor on 3ds.local, built by scripts/make-editor.sh into
// romfs/web/editor.js.gz. The page loads it the first time a file opens and
// keeps its plain text box when it cannot.
import {EditorState} from '@codemirror/state';
import {
  EditorView, keymap, lineNumbers, highlightActiveLine, highlightActiveLineGutter,
  highlightSpecialChars, drawSelection,
} from '@codemirror/view';
import {defaultKeymap, history, historyKeymap, indentWithTab} from '@codemirror/commands';
import {
  StreamLanguage, HighlightStyle, syntaxHighlighting, bracketMatching, indentUnit,
} from '@codemirror/language';
import {search, searchKeymap, highlightSelectionMatches} from '@codemirror/search';
import {tags as t} from '@lezer/highlight';
import {properties} from '@codemirror/legacy-modes/mode/properties';
import {javascript, json} from '@codemirror/legacy-modes/mode/javascript';
import {xml, html} from '@codemirror/legacy-modes/mode/xml';
import {lua} from '@codemirror/legacy-modes/mode/lua';
import {yaml} from '@codemirror/legacy-modes/mode/yaml';
import {shell} from '@codemirror/legacy-modes/mode/shell';
import {python} from '@codemirror/legacy-modes/mode/python';
import {css} from '@codemirror/legacy-modes/mode/css';

// By extension; anything else is plain text.
const MODES = {
  ini: properties, cfg: properties, conf: properties, opt: properties,
  json, xml, html, htm: html, lua, yml: yaml, yaml, sh: shell, py: python,
  js: javascript, css,
};

// The page's own colours, so the editor reads as part of it.
const theme = EditorView.theme({
  '&': {
    height: '55vh', minHeight: '180px', color: 'var(--text)', backgroundColor: 'var(--cream)',
    border: '1px solid #b3aa83', borderRadius: '10px', overflow: 'hidden',
    transition: 'border-color .2s,box-shadow .2s',
  },
  '&.cm-focused': {outline: 'none', borderColor: 'var(--blue)', boxShadow: '0 0 0 3px #1c93c233'},
  '.cm-scroller': {font: '13px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace'},
  '.cm-content': {padding: '10px 0', caretColor: 'var(--deep)'},
  '.cm-line': {padding: '0 14px 0 10px'},
  '.cm-gutters': {backgroundColor: '#f4efd8', color: '#9a9070', border: 'none', borderRight: '1px solid var(--line)'},
  '.cm-lineNumbers .cm-gutterElement': {padding: '0 8px 0 12px', minWidth: '28px'},
  '.cm-activeLine': {backgroundColor: '#fff3c480'},
  '.cm-activeLineGutter': {backgroundColor: 'var(--pick)', color: 'var(--ink)'},
  '.cm-cursor': {borderLeftColor: 'var(--deep)', borderLeftWidth: '2px'},
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection': {backgroundColor: '#1c93c233 !important'},
  '.cm-selectionMatch': {backgroundColor: '#e2ad3a40'},
  '.cm-searchMatch': {backgroundColor: '#e2ad3a55', outline: '1px solid #e2ad3a'},
  '.cm-searchMatch.cm-searchMatch-selected': {backgroundColor: '#1c93c255'},
  '&.cm-focused .cm-matchingBracket': {backgroundColor: '#3fa65a33', outline: '1px solid #3fa65a88'},
  '&.cm-focused .cm-nonmatchingBracket': {backgroundColor: '#e0301a33'},
  '.cm-panels': {backgroundColor: '#f4efd8', color: 'var(--ink)'},
  '.cm-panels.cm-panels-bottom': {borderTop: '1px solid var(--line)'},
  '.cm-panels.cm-panels-top': {borderBottom: '1px solid var(--line)'},
  '.cm-search': {padding: '8px 10px', fontSize: '13px', display: 'flex', flexWrap: 'wrap', gap: '6px', alignItems: 'center'},
  '.cm-search br': {flexBasis: '100%', height: 0},
  '.cm-search label': {display: 'inline-flex', alignItems: 'center', gap: '4px'},
  '.cm-textfield': {
    font: 'inherit', fontSize: '13px', padding: '4px 8px', margin: 0, borderRadius: '6px',
    border: '1px solid #b3aa83', backgroundColor: 'var(--cream)', color: 'var(--text)', width: 'auto',
  },
  '.cm-button': {
    font: 'inherit', fontSize: '13px', padding: '4px 10px', margin: 0, borderRadius: '6px',
    border: '1px solid #b3aa83', backgroundImage: 'linear-gradient(#fffdf4,#efe8c8)', color: 'var(--ink)',
  },
  '.cm-panel.cm-search [name=close]': {top: '6px', right: '8px', fontSize: '18px', color: 'var(--soft)'},
});

const highlight = HighlightStyle.define([
  {tag: [t.comment, t.lineComment, t.blockComment], color: '#8a8163', fontStyle: 'italic'},
  {tag: [t.keyword, t.controlKeyword, t.operatorKeyword, t.definitionKeyword, t.modifier], color: '#0f6f96', fontWeight: '600'},
  {tag: [t.string, t.special(t.string), t.quote], color: '#3d7a2a'},
  {tag: [t.number, t.bool, t.null, t.atom], color: '#a65200'},
  {tag: [t.propertyName, t.attributeName, t.definition(t.variableName)], color: '#3f7a91'},
  {tag: [t.tagName, t.typeName, t.className], color: '#b8240d'},
  {tag: t.heading, color: '#b8240d', fontWeight: '600'},
  {tag: [t.meta, t.processingInstruction, t.standard(t.variableName)], color: '#7a6a9a'},
  {tag: [t.link, t.url], color: '#0f6f96', textDecoration: 'underline'},
  {tag: t.invalid, color: '#e0301a'},
]);

// The search panel's words, cased like the page's buttons.
const phrases = EditorState.phrases.of({
  next: 'Next', previous: 'Previous', all: 'All', 'match case': 'Match Case',
  regexp: 'Regex', 'by word': 'Whole Word', replace: 'Replace', 'replace all': 'Replace All',
  close: 'Close',
});

// Opens text in parent. name picks the language; change runs after each
// edit, save on Ctrl or Cmd and S.
export function create({parent, text, name, change, save}) {
  const ext = (/\.([^./]+)$/.exec(name) || [])[1];
  const mode = MODES[(ext || '').toLowerCase()];
  const view = new EditorView({
    parent,
    state: EditorState.create({
      doc: text,
      extensions: [
        lineNumbers(), highlightActiveLineGutter(), highlightSpecialChars(), history(),
        drawSelection(), highlightActiveLine(), bracketMatching(), highlightSelectionMatches(),
        search({top: true}), EditorView.lineWrapping, EditorState.tabSize.of(4), indentUnit.of('    '),
        mode ? StreamLanguage.define(mode) : [], syntaxHighlighting(highlight), theme, phrases,
        EditorView.contentAttributes.of({spellcheck: 'false', autocorrect: 'off', autocapitalize: 'off'}),
        EditorView.updateListener.of(update => { if (update.docChanged) change(); }),
        keymap.of([
          {key: 'Mod-s', preventDefault: true, run: () => { save(); return true; }},
          ...defaultKeymap, ...historyKeymap, ...searchKeymap, indentWithTab,
        ]),
      ],
    }),
  });
  return {
    text: () => view.state.doc.toString(),
    lines: () => view.state.doc.lines,
    focus: () => view.focus(),
    destroy: () => view.destroy(),
  };
}
