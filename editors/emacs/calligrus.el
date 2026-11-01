;;; calligrus.el --- Live preview of Markdown and Org buffers in Calligrus  -*- lexical-binding: t; -*-

;; Copyright (C) 2026 KαΩSS (theKAOSSphere)
;; Author: KαΩSS (theKAOSSphere)
;; URL: https://github.com/theKAOSSphere/calligrus
;; Version: 0.2.0
;; Package-Requires: ((emacs "27.1"))
;; Keywords: markdown, outlines, convenience, tools
;; SPDX-License-Identifier: GPL-3.0-or-later

;; This file is not part of GNU Emacs.

;; This program is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with this program.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; Drives `calligrus --editor' (the calligrus executable): one shared preview
;; window per Emacs session shows whichever previewed buffer is current.
;; Protocol (spec sections 15 and 17): one JSON object per line on calligrus's
;; stdin.
;;
;;   open   {doc, path, text}    show this buffer now
;;   text   {doc, text}          the buffer changed
;;   cursor {doc, line}          scroll the preview to this source line
;;   search {doc, text, case}    highlight this plain text ("" = clear)
;;   close  {doc}                the buffer is gone
;;   quit                        exit
;;
;; calligrus talks back on its stdout (spec sections 23 and 26):
;;
;;   goto   {doc, line}                  the user Ctrl+clicked in the preview
;;   notes  {doc, slide, total, notes, line, end}
;;                                       slides mode shows a new slide
;;
;; and then the cursor jumps to that line (`calligrus-goto'), or to the slide
;; (lines LINE..END; `calligrus-follow-slides') and the slide's speaker notes go
;; to the `*calligrus notes*' buffer (`calligrus-notes').  Apart from that
;; this file only sends messages when Emacs events happen.  Commands:
;;
;;   M-x calligrus-toggle     preview the current buffer on/off
;;   M-x calligrus-stop       quit the calligrus process (all previews off)
;;   M-x calligrus-open-file  open the buffer's file in a separate, unlinked calligrus
;;
;; `calligrus-preview-mode' is the buffer-local minor mode behind `calligrus-toggle'.
;; While the preview runs and `calligrus-follow' is non-nil, every Markdown
;; buffer you visit joins in; a buffer you toggled off stays muted.

;;; Code:

(require 'subr-x)
(require 'cl-lib)

(defgroup calligrus nil
  "Live preview of Markdown buffers in calligrus."
  :group 'tools
  :prefix "calligrus-")

(defcustom calligrus-exe "calligrus"
  "The calligrus program: a string, or a list (PROGRAM ARG...) of leading arguments.
`--editor' is appended when the shared process is started."
  :type '(choice string (repeat string)))

(defcustom calligrus-throttle 0.15
  "While typing, send the whole text at most this often (seconds).
The last edit is always sent."
  :type 'number)

(defcustom calligrus-cursor-delay 0.05
  "Send the cursor line this long (seconds) after the last cursor movement."
  :type 'number)

(defcustom calligrus-pipe-buffer-size (* 4 1024 1024)
  "Size in bytes of the pipe to calligrus (Windows only; ignored elsewhere).
Emacs' default pipe is tiny and sending a 300 KB buffer through it takes about
a second of frozen Emacs; with a 1 MB or larger pipe it takes ~8 ms.  Make it
larger than your biggest document."
  :type 'integer)

(defcustom calligrus-follow t
  "If non-nil, once the preview runs every previewable buffer you visit joins.
A buffer you toggled off (see `calligrus-toggle') is left alone."
  :type 'boolean)

(defcustom calligrus-goto t
  "If non-nil, Ctrl+click in the preview moves point to that source line.
nil: ignore the preview's `goto' messages."
  :type 'boolean)

(defcustom calligrus-goto-focus t
  "If non-nil, a Ctrl+click in the preview also brings Emacs back to the front.
nil: point moves, but the preview window keeps the keyboard focus."
  :type 'boolean)

(defcustom calligrus-notes t
  "If non-nil, show the speaker notes of the slide on screen in a side buffer.
In slides mode (F5) calligrus sends each slide's notes (`<!-- speaker_note: ... -->'
in Markdown, a #+begin_notes block in Org); they go to the buffer named by
`calligrus-notes-buffer', which pops up the first time a slide has notes.
nil: ignore them."
  :type 'boolean)

(defcustom calligrus-follow-slides t
  "If non-nil, in slides mode point follows the slide on screen.
When the preview moves to another slide, point goes to the slide's first line,
unless it is somewhere on that slide already.  nil: point stays put."
  :type 'boolean)

(defcustom calligrus-notes-buffer "*calligrus notes*"
  "Name of the buffer that shows the speaker notes (see `calligrus-notes')."
  :type 'string)

(defcustom calligrus-modes '(markdown-mode gfm-mode markdown-ts-mode org-mode)
  "Major modes whose buffers can be previewed (derived modes count)."
  :type '(repeat symbol))

(defcustom calligrus-keep-isearch-highlight 'auto
  "Whether the preview keeps highlighting an isearch string after isearch ends.
nil: clear when isearch ends (like Emacs itself).  t: keep it (like Vim's
hlsearch).  `auto': keep it when Evil uses its isearch search module."
  :type '(choice (const :tag "Auto (keep with Evil isearch)" auto)
                 (const :tag "Keep" t)
                 (const :tag "Clear" nil)))

;;;; State

(defvar calligrus--process nil "The running calligrus process, or nil.")
(defvar calligrus--shown nil "The buffer last announced to calligrus with `open', or nil.")
(defvar calligrus--search '("" . nil)
  "The search calligrus should highlight: (TEXT . CASE-SENSITIVE-P).")
(defvar calligrus--doc-counter 0)
(defvar calligrus--out-partial ""
  "The unfinished last line of what calligrus wrote to its stdout so far.")

(defvar-local calligrus--doc-id nil "This buffer's document id in the protocol.")
(defvar-local calligrus--muted nil
  "Non-nil: the user toggled this buffer off; `calligrus-follow' skips it.")
(defvar-local calligrus--text-timer nil)
(defvar-local calligrus--cursor-timer nil)
(defvar-local calligrus--last-line nil "Last cursor line sent for this buffer.")
(defvar-local calligrus--last-search nil "Last search key sent for this buffer.")

(defvar calligrus-preview-mode)            ; defined by the minor mode below
(defvar w32-pipe-buffer-size)           ; Emacs on Windows only

(defconst calligrus--hooks
  '((after-change-functions . calligrus--after-change)
    (post-command-hook . calligrus--post-command)
    (kill-buffer-hook . calligrus--kill-buffer)
    (kill-emacs-hook . calligrus--kill-emacs)
    (window-buffer-change-functions . calligrus--visit)
    (window-selection-change-functions . calligrus--visit)
    (after-change-major-mode-hook . calligrus--visit)
    (isearch-update-post-hook . calligrus--isearch-update)
    (isearch-mode-end-hook . calligrus--isearch-end))
  "Global hooks installed while the process runs.")

;;;; Process

(defun calligrus--command ()
  "The command line that starts calligrus in editor mode."
  (append (if (listp calligrus-exe) calligrus-exe (list calligrus-exe)) (list "--editor")))

(defun calligrus--alive-p ()
  (and calligrus--process (process-live-p calligrus--process)))

(defun calligrus--send (msg)
  "Send plist MSG as one JSON line.  Return non-nil if it was written."
  (when (calligrus--alive-p)
    (let ((line (condition-case nil (json-serialize msg) (error nil))))
      (when line
        (condition-case nil
            (progn (process-send-string calligrus--process (concat line "\n")) t)
          (error nil))))))

(defun calligrus--install-hooks ()
  (dolist (h calligrus--hooks) (add-hook (car h) (cdr h))))

(defun calligrus--remove-hooks ()
  (dolist (h calligrus--hooks) (remove-hook (car h) (cdr h))))

(defun calligrus--cancel-timers ()
  (when (timerp calligrus--text-timer) (cancel-timer calligrus--text-timer))
  (when (timerp calligrus--cursor-timer) (cancel-timer calligrus--cursor-timer))
  (setq calligrus--text-timer nil calligrus--cursor-timer nil))

(defun calligrus--forget-all ()
  "Turn every preview off WITHOUT telling calligrus (it is gone or going)."
  (dolist (buf (buffer-list))
    (with-current-buffer buf
      (when calligrus-preview-mode
        (setq calligrus-preview-mode nil))
      (calligrus--cancel-timers)
      (setq calligrus--last-line nil calligrus--last-search nil)))
  (setq calligrus--shown nil calligrus--search '("" . nil))
  (calligrus--remove-hooks)
  (force-mode-line-update t))

(defun calligrus--sentinel (proc _event)
  (when (and (eq proc calligrus--process) (not (process-live-p proc)))
    (let ((code (process-exit-status proc)))
      (setq calligrus--process nil)
      (calligrus--forget-all)
      (message "calligrus: the preview window closed (exit code %s); previews are off" code))))

(defun calligrus--filter (_proc string)
  "Process filter: collect calligrus's stdout and handle each complete line.
STRING arrives in arbitrary chunks, so the unfinished last line is kept."
  (let ((text (concat calligrus--out-partial string))
        (start 0) pos)
    (save-match-data
      (while (setq pos (string-match "\n" text start))
        (calligrus--handle-line (substring text start pos))
        (setq start (1+ pos))))
    (setq calligrus--out-partial (substring text start))))

(defun calligrus--handle-line (line)
  "Handle one line from calligrus.  Unknown or malformed input is ignored."
  (let ((msg (condition-case nil
                 (json-parse-string line :object-type 'alist)
               (error nil))))
    (when (consp msg)
      (pcase (alist-get 'type msg)
        ("goto" (when calligrus-goto (calligrus--handle-goto msg)))
        ("notes"
         (when calligrus-follow-slides (calligrus--follow-slide msg))
         (when calligrus-notes (calligrus--handle-notes msg)))))))

(defun calligrus--handle-goto (msg)
  "Move point to the line a `goto' message MSG names (Ctrl+click in the preview)."
  (let ((doc (alist-get 'doc msg))
        (line (alist-get 'line msg)))
    (when (and (stringp doc) (numberp line))
      (let ((buf (calligrus--doc-buffer doc)))
        (when buf
          ;; Never let a bad message break the process filter.
          (condition-case err
              (calligrus--goto buf (floor line))
            (error (message "calligrus: cannot go to line %s (%s)"
                            line (error-message-string err)))))))))

(defun calligrus--follow-slide (msg)
  "Put point on the slide a `notes' message MSG names (its lines LINE..END).
Nothing happens if point is on the slide already, or no window shows the buffer.
The preview is not told about this move: it is already there."
  (let* ((doc (alist-get 'doc msg))
         (line (alist-get 'line msg))
         (end (alist-get 'end msg))
         (buf (and (stringp doc) (calligrus--doc-buffer doc)))
         (win (and buf (get-buffer-window buf t))))
    (when (and win (numberp line) (numberp end))
      (with-selected-window win
        (save-restriction
          (widen)
          (unless (<= line (line-number-at-pos (point) t) end)
            (goto-char (point-min))
            (forward-line (1- (max 1 (floor line))))
            (calligrus--reveal)
            (setq calligrus--last-line (line-number-at-pos (point) t))))))))

(defun calligrus--handle-notes (msg)
  "Show the speaker notes of a `notes' message MSG (slides mode is on a new slide).
The notes buffer is filled every time; it is shown (without selecting it) once
a slide has notes, and stays where the user put it after that."
  (let ((slide (alist-get 'slide msg))
        (total (alist-get 'total msg))
        (notes (alist-get 'notes msg)))
    (when (and (numberp slide) (numberp total) (stringp notes))
      (let ((buf (get-buffer calligrus-notes-buffer)))
        (when (or buf (not (string-empty-p notes)))
          (setq buf (get-buffer-create calligrus-notes-buffer))
          (with-current-buffer buf
            (let ((inhibit-read-only t))
              (erase-buffer)
              (insert (propertize (format "Slide %d / %d" slide total) 'face 'bold) "\n\n"
                      (if (string-empty-p notes) "(no notes)" notes) "\n")
              (goto-char (point-min)))
            (setq buffer-read-only t))
          (unless (get-buffer-window buf t)
            (display-buffer buf '(nil (inhibit-same-window . t)))))))))

(defun calligrus--doc-buffer (doc)
  "The live buffer that is previewed under document id DOC, or nil."
  (cl-find-if (lambda (buf)
                (and (buffer-local-value 'calligrus-preview-mode buf)
                     (equal (buffer-local-value 'calligrus--doc-id buf) doc)))
              (buffer-list)))

(defun calligrus--goto (buf line)
  "Show BUF, put point on LINE (clamped, 1-based) at its indentation, reveal it.
Uses the window that shows BUF in the selected frame; else one on another
visible frame (selected only inside that frame); else the selected window."
  (let ((win (get-buffer-window buf nil)))
    (cond
     (win (select-window win))
     ((setq win (get-buffer-window buf 'visible)) nil)
     (t (switch-to-buffer buf)
        (setq win (selected-window))))
    (with-selected-window win
      (with-current-buffer buf
        (save-restriction
          (widen)
          (goto-char (point-min))
          (forward-line (1- (max 1 line)))
          ;; Past the end: stop on the last line, not on the empty one after the final newline.
          (when (and (eobp) (bolp) (not (bobp))) (forward-line -1)))
        (back-to-indentation)
        (calligrus--reveal)
        ;; Point of a window that isn't `selected-window' lives in the window.
        (set-window-point win (point))))
    (when (and calligrus-goto-focus (display-graphic-p (window-frame win)))
      (select-frame-set-input-focus (window-frame win)))
    ;; The command loop isn't running, so ask for the `cursor' message ourselves.
    (with-current-buffer buf (calligrus--post-command))))

(defun calligrus--reveal ()
  "Unfold whatever hides point, if that is cheap to do."
  (cond
   ((and (derived-mode-p 'org-mode) (fboundp 'org-reveal))
    (org-reveal))
   ((and (or (derived-mode-p 'outline-mode) (bound-and-true-p outline-minor-mode))
         (fboundp 'outline-show-entry))
    (outline-show-entry)))
  (when (and (bound-and-true-p reveal-mode) (fboundp 'reveal-post-command))
    (reveal-post-command)))

(defun calligrus--ensure-process ()
  "Start the shared process if needed.  Return non-nil if it is running."
  (or (calligrus--alive-p)
      (let ((cmd (calligrus--command)))
        (condition-case err
            (progn
              (setq calligrus--out-partial "")
              (setq calligrus--process
                    (let ((w32-pipe-buffer-size calligrus-pipe-buffer-size))
                     (make-process :name "calligrus"
                                  :command cmd
                                  :connection-type 'pipe
                                  :coding 'utf-8-unix
                                  :noquery t
                                  :buffer nil
                                  :filter #'calligrus--filter
                                  :stderr (get-buffer-create "*calligrus-log*")
                                  :sentinel #'calligrus--sentinel)))
              (set-process-query-on-exit-flag calligrus--process nil)
              (let ((errproc (get-buffer-process "*calligrus-log*")))
                (when errproc (set-process-query-on-exit-flag errproc nil)))
              (calligrus--install-hooks)
              t)
          (error
           (setq calligrus--process nil)
           (message "calligrus: cannot run `%s' (%s); see `calligrus-exe'"
                    (mapconcat #'identity cmd " ") (error-message-string err))
           nil)))))

;;;; Messages

(defun calligrus--buffer-text (buf)
  (with-current-buffer buf
    (save-restriction (widen) (buffer-substring-no-properties (point-min) (point-max)))))

(defun calligrus--doc (buf)
  (with-current-buffer buf
    (or calligrus--doc-id
        (setq calligrus--doc-id (number-to-string (cl-incf calligrus--doc-counter))))))

(defun calligrus--path (buf)
  (let ((file (buffer-local-value 'buffer-file-name buf)))
    (if (not file)
        ""
      (setq file (expand-file-name file))
      (if (eq system-type 'windows-nt) (subst-char-in-string ?/ ?\\ file) file))))

(defun calligrus--cursor-line (buf)
  "Cursor line (1-based) of the window showing BUF, or nil."
  (let ((win (get-buffer-window buf)))
    (when win
      (with-current-buffer buf
        (save-restriction (widen) (line-number-at-pos (window-point win) t))))))

(defun calligrus--send-open (buf)
  (setq calligrus--shown buf)
  (with-current-buffer buf (setq calligrus--last-line nil calligrus--last-search nil))
  (calligrus--send (list :type "open" :doc (calligrus--doc buf) :path (calligrus--path buf)
                      :format (if (with-current-buffer buf (derived-mode-p 'org-mode))
                                  "org" "markdown")
                      :text (calligrus--buffer-text buf))))

(defun calligrus--send-text (buf)
  (calligrus--send (list :type "text" :doc (calligrus--doc buf) :text (calligrus--buffer-text buf))))

(defun calligrus--send-cursor (buf)
  "Send BUF's cursor line, unless it is the one already sent."
  (let ((line (calligrus--cursor-line buf)))
    (when (and line (not (eql line (buffer-local-value 'calligrus--last-line buf))))
      (with-current-buffer buf (setq calligrus--last-line line))
      (calligrus--send (list :type "cursor" :doc (calligrus--doc buf) :line line)))))

(defun calligrus--send-search (buf &optional force)
  "Send the current search for BUF if it changed since last time (or FORCE)."
  (when (buffer-local-value 'calligrus-preview-mode buf)
    (let* ((text (car calligrus--search))
           (case-p (and (cdr calligrus--search) t))
           (key (cons text case-p)))
      (when (or force (not (equal key (buffer-local-value 'calligrus--last-search buf))))
        (with-current-buffer buf (setq calligrus--last-search key))
        (calligrus--send (list :type "search" :doc (calligrus--doc buf) :text text
                            :case (if case-p t :false)))))))

(defun calligrus--announce (buf)
  "Open BUF in calligrus and sync its cursor and search."
  (calligrus--send-open buf)
  (calligrus--send-cursor buf)
  (calligrus--send-search buf t))

;;;; Search patterns

(defun calligrus--plain-text (regexp)
  "Turn an Emacs REGEXP (as Evil builds it) into plain text, or \"\".
Word/symbol anchors are dropped; `regexp-quote'd characters are unquoted;
anything that is a real regexp gives \"\" (a wrong highlight is worse than none)."
  (let* ((s (replace-regexp-in-string "\\\\\\(?:_?[<>]\\|b\\|B\\)" "" regexp t t))
         (n (length s)) (i 0) (out nil) (ok t))
    (while (and ok (< i n))
      (let ((c (aref s i)))
        (cond
         ((eq c ?\\)
          (let ((d (and (< (1+ i) n) (aref s (1+ i)))))
            (if (and d (memq d '(?\\ ?. ?* ?+ ?? ?\[ ?\] ?^ ?$)))
                (progn (push d out) (setq i (+ i 2)))
              (setq ok nil))))
         ((memq c '(?. ?* ?+ ?? ?\[ ?\] ?^ ?$)) (setq ok nil))
         (t (push c out) (setq i (1+ i))))))
    (if ok (apply #'string (nreverse out)) "")))

(defun calligrus--set-search (text case-p)
  (setq calligrus--search (cons text case-p))
  (when (and (calligrus--alive-p) calligrus-preview-mode)
    (calligrus--send-search (current-buffer))))

;; isearch
(defvar isearch-string)
(defvar isearch-regexp)
(defvar isearch-case-fold-search)
(defvar isearch-update-post-hook)
(defvar isearch-mode-end-hook)

(defun calligrus--isearch-update ()
  (when (calligrus--alive-p)
    (if (or (not (stringp isearch-string)) (string= isearch-string "") isearch-regexp)
        (calligrus--set-search "" nil)
      (calligrus--set-search isearch-string (not isearch-case-fold-search)))))

(defun calligrus--keep-isearch-p ()
  (if (eq calligrus-keep-isearch-highlight 'auto)
      (and (bound-and-true-p evil-mode)
           (eq (bound-and-true-p evil-search-module) 'isearch))
    calligrus-keep-isearch-highlight))

(defun calligrus--isearch-end ()
  (when (and (calligrus--alive-p) (not (calligrus--keep-isearch-p)))
    (calligrus--set-search "" nil)))

;; Evil's own search (evil-search-module = evil-search) and :nohlsearch.
(defvar evil-ex-search-pattern)
(declare-function evil-ex-pattern-regex "evil-search")
(declare-function evil-ex-pattern-ignore-case "evil-search")
(declare-function evil-ex-hl-active-p "evil-search")

(defun calligrus--evil-search-changed (&rest _)
  (when (calligrus--alive-p)
    (let ((pat (and (boundp 'evil-ex-search-pattern) evil-ex-search-pattern)))
      (if (or (null pat)
              (and (fboundp 'evil-ex-hl-active-p) (not (evil-ex-hl-active-p 'evil-ex-search))))
          (calligrus--set-search "" nil)
        (let* ((regexp (if (fboundp 'evil-ex-pattern-regex) (evil-ex-pattern-regex pat) (car pat)))
               (fold (condition-case nil
                         ;; Evil patterns are (REGEXP IGNORE-CASE WHOLE-LINE).
                         (if (fboundp 'evil-ex-pattern-ignore-case)
                             (evil-ex-pattern-ignore-case pat)
                           (nth 1 pat))
                       (error case-fold-search)))
               (text (if (stringp regexp) (calligrus--plain-text regexp) "")))
          (calligrus--set-search text (and (not (string= text "")) (not fold))))))))

(defun calligrus--evil-nohighlight (&rest _)
  (when (calligrus--alive-p)
    (calligrus--set-search "" nil)))

(with-eval-after-load 'evil-search
  ;; `*' `#' `n' `N' go through evil-ex-search; a typed `/' or `?' goes
  ;; through evil-ex-start-search (it doesn't call evil-ex-search).
  (advice-add 'evil-ex-search :after #'calligrus--evil-search-changed)
  (advice-add 'evil-ex-start-search :after #'calligrus--evil-search-changed)
  (advice-add 'evil-ex-nohighlight :after #'calligrus--evil-nohighlight))

;;;; Event handlers

(defun calligrus--after-change (&rest _)
  "Throttle: the first edit starts a timer; later edits ride along."
  (when (and calligrus-preview-mode (not calligrus--text-timer))
    (let ((buf (current-buffer)))
      (setq calligrus--text-timer
            (run-with-timer calligrus-throttle nil #'calligrus--text-fire buf)))))

(defun calligrus--text-fire (buf)
  "Timer: send BUF's text as it is NOW (so the last edit always goes out)."
  (when (buffer-live-p buf)
    (with-current-buffer buf
      (setq calligrus--text-timer nil)
      (when calligrus-preview-mode (calligrus--send-text buf)))))

(defun calligrus--post-command ()
  "Debounce the cursor line."
  (when calligrus-preview-mode
    (when (timerp calligrus--cursor-timer) (cancel-timer calligrus--cursor-timer))
    (let ((buf (current-buffer)))
      (setq calligrus--cursor-timer
            (run-with-timer calligrus-cursor-delay nil #'calligrus--cursor-fire buf)))))

(defun calligrus--cursor-fire (buf)
  (when (buffer-live-p buf)
    (with-current-buffer buf
      (setq calligrus--cursor-timer nil)
      (when calligrus-preview-mode (calligrus--send-cursor buf)))))

(defun calligrus--previewable-p (&optional buf)
  (with-current-buffer (or buf (current-buffer))
    (apply #'derived-mode-p calligrus-modes)))

(defun calligrus--visit (&rest _)
  "The selected window may show a new buffer: follow it / announce it."
  (when (calligrus--alive-p)
    (let ((buf (window-buffer (selected-window))))
      (unless (minibufferp buf)
        (with-current-buffer buf
          (when (and calligrus-follow (not calligrus-preview-mode) (not calligrus--muted)
                     (calligrus--previewable-p))
            (calligrus-preview-mode 1)))
        (if (buffer-local-value 'calligrus-preview-mode buf)
            (unless (eq buf calligrus--shown) (calligrus--announce buf))
          (setq calligrus--shown nil))))))

(defun calligrus--kill-buffer ()
  (when calligrus-preview-mode
    (calligrus--send (list :type "close" :doc (calligrus--doc (current-buffer))))
    (calligrus--cancel-timers)
    (when (eq calligrus--shown (current-buffer)) (setq calligrus--shown nil))))

(defun calligrus--kill-emacs ()
  (calligrus-stop))

;;;; Commands

;;;###autoload
(define-minor-mode calligrus-preview-mode
  "Preview this buffer live in the shared calligrus window.
Normally turned on by `calligrus-toggle'."
  :lighter " MdV"
  :group 'calligrus
  (if calligrus-preview-mode
      (if (not (calligrus--ensure-process))
          (setq calligrus-preview-mode nil)
        (setq calligrus--muted nil)
        (calligrus--announce (current-buffer)))
    (when (calligrus--alive-p)
      (calligrus--send (list :type "close" :doc (calligrus--doc (current-buffer)))))
    (calligrus--cancel-timers)
    (when (eq calligrus--shown (current-buffer)) (setq calligrus--shown nil))))

;;;###autoload
(defun calligrus-toggle (&optional force)
  "Turn the live preview of the current buffer on or off.
With prefix argument FORCE, preview even if the major mode is not in
`calligrus-modes'.
Turning a buffer off mutes it: `calligrus-follow' won't bring it back."
  (interactive "P")
  (cond
   (calligrus-preview-mode
    (calligrus-preview-mode -1)
    (setq calligrus--muted t)
    (message "calligrus: preview off"))
   ((and (not force) (not (calligrus--previewable-p)))
    (message "calligrus: major mode is not one of %s (use C-u M-x calligrus-toggle to preview anyway)"
             (mapconcat #'symbol-name calligrus-modes ", ")))
   (t (calligrus-preview-mode 1))))

;;;###autoload
(defun calligrus-stop ()
  "Quit the calligrus process; all previews turn off."
  (interactive)
  (let ((proc calligrus--process))
    (when proc
      (setq calligrus--process nil)   ; the sentinel stays quiet: we asked for it
      (when (process-live-p proc)
        (ignore-errors
          (process-send-string proc (concat (json-serialize '(:type "quit")) "\n"))
          ;; Closing stdin also makes calligrus exit; don't wait for it to read `quit'.
          (process-send-eof proc)))
      (calligrus--forget-all))))

;;;###autoload
(defun calligrus-open-file ()
  "Open the current buffer's file in a new, unconnected calligrus window."
  (interactive)
  (if (not buffer-file-name)
      (message "calligrus: this buffer has no file")
    (let ((cmd (append (if (listp calligrus-exe) calligrus-exe (list calligrus-exe))
                       (list (calligrus--path (current-buffer))))))
      (condition-case err
          ;; Destination 0: start it detached and don't wait.
          (apply #'call-process (car cmd) nil 0 nil (cdr cmd))
        (error (message "calligrus: cannot run `%s' (%s)"
                        (mapconcat #'identity cmd " ") (error-message-string err)))))))

(provide 'calligrus)
;;; calligrus.el ends here
