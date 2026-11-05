;;; calligrus-test.el --- ERT tests for calligrus.el  -*- lexical-binding: t; -*-

;; Run:  emacs --batch -Q -l test/calligrus-test.el -f ert-run-tests-batch-and-exit
;; Uses test/stub.py (python) as a stand-in for `calligrus --editor'; it appends every
;; stdin line to the file named by $CALLIGRUS_STUB_LOG.  No window is ever shown.

(require 'ert)
(require 'cl-lib)

(defconst calligrus-test--dir (file-name-directory (or load-file-name buffer-file-name)))
(load (expand-file-name "../calligrus.el" calligrus-test--dir) nil t)

(defconst calligrus-test--python (or (executable-find "python3") (executable-find "python")))
(defvar calligrus-test--log nil)
(defvar calligrus-test--out nil
  "File whose new lines the stub writes to its stdout (what the app's `goto' looks like).")

;; A stand-in major mode so `calligrus-modes' matches without markdown-mode installed.
(define-derived-mode markdown-mode text-mode "MD")

(defun calligrus-test--lines ()
  (when (file-exists-p calligrus-test--log)
    (with-temp-buffer
      (let ((coding-system-for-read 'utf-8-unix))
        (insert-file-contents calligrus-test--log))
      (split-string (buffer-string) "\n" t))))

(defun calligrus-test--msgs ()
  "Protocol messages received by the stub so far, as alists."
  (cl-loop for l in (calligrus-test--lines)
           when (string-prefix-p "{" l)
           collect (json-parse-string l :object-type 'alist :false-object :false)))

(defun calligrus-test--of (type)
  (cl-remove-if-not (lambda (m) (equal (alist-get 'type m) type)) (calligrus-test--msgs)))

(defun calligrus-test--count (type) (length (calligrus-test--of type)))
(defun calligrus-test--last () (car (last (calligrus-test--msgs))))

(defun calligrus-test--wait (pred &optional secs)
  "Run timers/process output until PRED is non-nil (default 3 s)."
  (let ((end (+ (float-time) (or secs 3))))
    (while (and (not (funcall pred)) (< (float-time) end))
      (accept-process-output nil 0.02))
    (funcall pred)))

(defun calligrus-test--settle (&optional secs) (accept-process-output nil (or secs 0.4)))

(defmacro calligrus-test--session (&rest body)
  "Run BODY with a fresh stub log, a stub as `calligrus-exe', and clean-up afterwards."
  (declare (indent 0))
  `(let* ((calligrus-test--log (make-temp-file "calligrus-stub" nil ".log"))
          (calligrus-test--out (concat calligrus-test--log ".out"))
          (process-environment (append (list (concat "CALLIGRUS_STUB_LOG=" calligrus-test--log)
                                             (concat "CALLIGRUS_STUB_OUT=" calligrus-test--out))
                                       process-environment))
          (calligrus-exe (list calligrus-test--python (expand-file-name "stub.py" calligrus-test--dir)))
          (calligrus-throttle 0.05)
          (calligrus-cursor-delay 0.02)
          (calligrus-follow t)
          (calligrus-keep-isearch-highlight nil))
     (delete-file calligrus-test--log)
     (ignore-errors (delete-file calligrus-test--out))
     (unwind-protect (progn ,@body)
       (calligrus-stop)
       (dolist (b (buffer-list))
         (when (string-prefix-p "cg-t" (buffer-name b))
           (let ((kill-buffer-hook nil)) (kill-buffer b))))
       (delete-other-windows)
       (ignore-errors (delete-file calligrus-test--log))
       (ignore-errors (delete-file calligrus-test--out)))))


(defun calligrus-test--buffer (name text &optional mode)
  "A buffer shown in the selected window with TEXT (cursor at the end)."
  (let ((buf (generate-new-buffer (concat "cg-t-" name))))
    (switch-to-buffer buf)
    (insert text)
    (funcall (or mode #'markdown-mode))
    buf))

(defconst calligrus-test--sample "# Title\n\nline three \"quoted\" é ✓\n\nlast")

(defun calligrus-test--start ()
  "A markdown buffer with the sample text, preview toggled on and open received."
  (let ((buf (calligrus-test--buffer "a" calligrus-test--sample)))
    (calligrus-toggle)
    (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "open") 1))))
    buf))

;;; Tests

(ert-deftest calligrus-test-command-forms ()
  (let ((calligrus-exe "calligrus.exe"))
    (should (equal (calligrus--command) '("calligrus.exe" "--editor"))))
  (let ((calligrus-exe '("python" "x.py")))
    (should (equal (calligrus--command) '("python" "x.py" "--editor")))))

(ert-deftest calligrus-test-open ()
  (calligrus-test--session
    (let ((buf (calligrus-test--start)))
      (should (equal (car (calligrus-test--lines)) "ARGS --editor"))
      (let ((open (car (calligrus-test--of "open"))))
        (should (equal (alist-get 'doc open) (calligrus--doc buf)))
        (should (equal (alist-get 'path open) ""))
        (should (equal (alist-get 'format open) "markdown"))
        (should (equal (alist-get 'text open) calligrus-test--sample)))
      (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "cursor") 1))))
      (should (= (alist-get 'line (car (calligrus-test--of "cursor"))) 5))
      (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "search") 1))))
      (should (equal (alist-get 'text (car (calligrus-test--of "search"))) "")))))

(ert-deftest calligrus-test-open-has-path ()
  (calligrus-test--session
    (let ((file (make-temp-file "cg-sample" nil ".md")))
      (unwind-protect
          (progn
            (find-file file)
            (rename-buffer "cg-t-file")
            (insert "hello")
            (markdown-mode)
            (calligrus-toggle)
            (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "open") 1))))
            (let ((p (alist-get 'path (car (calligrus-test--of "open")))))
              (should (string-match-p "cg-sample.*\\.md\\'" p))
              (when (eq system-type 'windows-nt) (should-not (string-match-p "/" p)))))
        (set-buffer-modified-p nil)
        (let ((kill-buffer-hook nil)) (kill-buffer "cg-t-file"))
        (delete-file file)))))

(ert-deftest calligrus-test-org-format ()
  (calligrus-test--session
    (with-current-buffer (get-buffer-create "cg-t-org")
      (insert "* Heading

Text.
")
      (org-mode)
      (calligrus-toggle)
      (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "open") 1))))
      (should (equal (alist-get 'format (car (calligrus-test--of "open"))) "org")))))

(ert-deftest calligrus-test-cursor ()
  (calligrus-test--session
    (calligrus-test--start)
    (calligrus-test--wait (lambda () (>= (calligrus-test--count "cursor") 1)))
    (goto-char (point-min)) (forward-line 2)
    (calligrus--post-command)
    (should (calligrus-test--wait (lambda () (equal (alist-get 'line (calligrus-test--last)) 3))))
    (should (equal (alist-get 'type (calligrus-test--last)) "cursor"))
    ;; rapid moves: only the last line is sent
    (let ((n (calligrus-test--count "cursor")))
      (dotimes (i 4) (goto-char (point-min)) (forward-line i) (calligrus--post-command))
      (calligrus-test--settle)
      (should (= (calligrus-test--count "cursor") (1+ n)))
      (should (= (alist-get 'line (calligrus-test--last)) 4)))))

(ert-deftest calligrus-test-text-throttled ()
  (calligrus-test--session
    (calligrus-test--start)
    (calligrus-test--settle 0.2)
    ;; three quick edits -> one message with the final text
    (let ((before (calligrus-test--count "text")))
      (dotimes (i 3)
        (goto-char (point-max)) (insert (format " e%d" i)))
      (should (calligrus-test--wait (lambda () (> (calligrus-test--count "text") before))))
      (calligrus-test--settle)
      (should (= (calligrus-test--count "text") (1+ before)))
      (should (string-suffix-p "e0 e1 e2" (alist-get 'text (calligrus-test--last)))))
    ;; continuous typing for ~0.5 s: updates keep coming (throttle, not debounce),
    ;; but far fewer than keystrokes, and the last edit is always sent
    (let ((before (calligrus-test--count "text")) (keys 0))
      (let ((end (+ (float-time) 0.5)))
        (while (< (float-time) end)
          (goto-char (point-max)) (insert "x") (cl-incf keys)
          (accept-process-output nil 0.01)))
      (calligrus-test--settle)
      (let ((sent (- (calligrus-test--count "text") before)))
        (should (>= sent 3))
        (should (< sent keys)))
      (should (equal (alist-get 'text (calligrus-test--last)) (buffer-string))))))

(ert-deftest calligrus-test-visit-and-follow ()
  (calligrus-test--session
    (let ((a (calligrus-test--start)))
      (calligrus-test--settle 0.1)
      ;; a non-markdown buffer: preview not touched; coming back re-opens
      (let ((plain (calligrus-test--buffer "plain" "x" #'fundamental-mode))
            (opens (calligrus-test--count "open")))
        (calligrus--visit)
        (should (= (calligrus-test--count "open") opens))
        (should-not (buffer-local-value 'calligrus-preview-mode plain))
        (switch-to-buffer a)
        (calligrus--visit)
        (should (calligrus-test--wait (lambda () (= (calligrus-test--count "open") (1+ opens)))))
        ;; repeated hook calls for the same buffer don't resend
        (calligrus--visit) (calligrus--visit)
        (calligrus-test--settle 0.1)
        (should (= (calligrus-test--count "open") (1+ opens))))
      ;; follow: a new markdown buffer joins by itself
      (let ((opens (calligrus-test--count "open"))
            (b (calligrus-test--buffer "b" "# other")))
        (calligrus--visit)
        (should (calligrus-test--wait (lambda () (= (calligrus-test--count "open") (1+ opens)))))
        (should (buffer-local-value 'calligrus-preview-mode b))
        (should (equal (alist-get 'text (car (last (calligrus-test--of "open")))) "# other"))
        ;; mute: toggled off -> close, and visiting it again does nothing
        (calligrus-toggle)
        (should (calligrus-test--wait (lambda () (equal (alist-get 'type (calligrus-test--last)) "close"))))
        (should (equal (alist-get 'doc (calligrus-test--last)) (calligrus--doc b)))
        (switch-to-buffer a) (calligrus--visit)
        (switch-to-buffer b) (calligrus--visit)
        (let ((opens (calligrus-test--count "open")) (texts (calligrus-test--count "text")))
          (goto-char (point-max)) (insert " edit")
          (calligrus-test--settle)
          (should-not (buffer-local-value 'calligrus-preview-mode b))
          (should (= (calligrus-test--count "open") opens))
          (should (= (calligrus-test--count "text") texts)))
        ;; toggling on again unmutes
        (calligrus-toggle)
        (should (buffer-local-value 'calligrus-preview-mode b))))))

(ert-deftest calligrus-test-follow-off ()
  (calligrus-test--session
    (calligrus-test--start)
    (let ((calligrus-follow nil) (opens (calligrus-test--count "open")))
      (let ((b (calligrus-test--buffer "b" "# other")))
        (calligrus--visit)
        (calligrus-test--settle 0.2)
        (should-not (buffer-local-value 'calligrus-preview-mode b))
        (should (= (calligrus-test--count "open") opens))))))

(ert-deftest calligrus-test-toggle-refuses-other-modes ()
  (calligrus-test--session
    (calligrus-test--buffer "plain" "x" #'fundamental-mode)
    (calligrus-toggle)
    (should-not calligrus-preview-mode)
    (should-not (calligrus--alive-p))
    (calligrus-toggle t)                   ; forced
    (should calligrus-preview-mode)
    (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "open") 1))))))

(ert-deftest calligrus-test-search-plain-text ()
  (should (equal (calligrus--plain-text "foo") "foo"))
  (should (equal (calligrus--plain-text "\\<foo\\>") "foo"))
  (should (equal (calligrus--plain-text "\\_<foo\\_>") "foo"))
  (should (equal (calligrus--plain-text "a\\.b") "a.b"))
  (should (equal (calligrus--plain-text "a.b") ""))
  (should (equal (calligrus--plain-text "fo+") ""))
  (should (equal (calligrus--plain-text "^foo") ""))
  (should (equal (calligrus--plain-text "\\(a\\|b\\)") ""))
  (should (equal (calligrus--plain-text "[ab]") "")))

(ert-deftest calligrus-test-search-isearch ()
  (calligrus-test--session
    (calligrus-test--start)
    (calligrus-test--settle 0.1)
    (let ((isearch-string "Foo") (isearch-regexp nil) (isearch-case-fold-search nil))
      (run-hooks 'isearch-update-post-hook)
      (let ((m (calligrus-test--last)))
        (should (equal (alist-get 'type m) "search"))
        (should (equal (alist-get 'text m) "Foo"))
        (should (eq (alist-get 'case m) t)))
      (setq isearch-string "foo" isearch-case-fold-search t)
      (run-hooks 'isearch-update-post-hook)
      (should (eq (alist-get 'case (calligrus-test--last)) :false))
      (should (equal (alist-get 'text (calligrus-test--last)) "foo"))
      ;; unchanged -> not resent
      (let ((n (length (calligrus-test--msgs))))
        (run-hooks 'isearch-update-post-hook)
        (calligrus-test--settle 0.1)
        (should (= n (length (calligrus-test--msgs)))))
      ;; regexp search -> clear
      (setq isearch-regexp t)
      (run-hooks 'isearch-update-post-hook)
      (should (equal (alist-get 'text (calligrus-test--last)) ""))
      ;; end of isearch clears
      (setq isearch-regexp nil)
      (run-hooks 'isearch-update-post-hook)
      (should (equal (alist-get 'text (calligrus-test--last)) "foo"))
      (run-hooks 'isearch-mode-end-hook)
      (should (equal (alist-get 'text (calligrus-test--last)) ""))
      ;; keep option: the highlight stays; a re-opened buffer gets it
      (let ((calligrus-keep-isearch-highlight t))
        (run-hooks 'isearch-update-post-hook)
        (run-hooks 'isearch-mode-end-hook)
        (should (equal (alist-get 'text (calligrus-test--last)) "foo"))
        (calligrus--send-open (current-buffer))
        (calligrus--send-search (current-buffer) t)
        (should (equal (alist-get 'text (calligrus-test--last)) "foo"))))))

(ert-deftest calligrus-test-search-evil ()
  (calligrus-test--session
    (calligrus-test--start)
    (calligrus-test--settle 0.1)
    ;; Fake the bits of Evil we use, then "load" evil-search so the advice goes in.
    (defvar evil-ex-search-pattern nil)
    (cl-letf (((symbol-function 'evil-ex-search) (lambda (&rest _) t))
              ((symbol-function 'evil-ex-nohighlight) (lambda (&rest _) t)))
      (provide 'evil-search)
      (unwind-protect
          (progn
            (setq evil-ex-search-pattern '("\\<word\\>" t nil))
            (evil-ex-search)
            (should (calligrus-test--wait (lambda () (equal (alist-get 'text (calligrus-test--last)) "word"))))
            (setq evil-ex-search-pattern '("a.*b" t nil))
            (evil-ex-search)
            (should (equal (alist-get 'text (calligrus-test--last)) ""))
            (setq evil-ex-search-pattern '("word" t nil))
            (evil-ex-search)
            (should (equal (alist-get 'text (calligrus-test--last)) "word"))
            (evil-ex-nohighlight)
            (should (equal (alist-get 'text (calligrus-test--last)) "")))
        (advice-remove 'evil-ex-search #'calligrus--evil-search-changed)
        (advice-remove 'evil-ex-nohighlight #'calligrus--evil-nohighlight)))))

(ert-deftest calligrus-test-close-on-kill-buffer ()
  (calligrus-test--session
    (let ((buf (calligrus-test--start)) doc)
      (setq doc (calligrus--doc buf))
      (kill-buffer buf)
      (should (calligrus-test--wait (lambda () (= (calligrus-test--count "close") 1))))
      (should (equal (alist-get 'doc (car (calligrus-test--of "close"))) doc)))))

(ert-deftest calligrus-test-quit-on-kill-emacs ()
  (calligrus-test--session
    (calligrus-test--start)
    (should (memq 'calligrus--kill-emacs kill-emacs-hook))
    (funcall 'calligrus--kill-emacs)
    (should (calligrus-test--wait (lambda () (= (calligrus-test--count "quit") 1))))
    (should (calligrus-test--wait (lambda () (member "EOF" (calligrus-test--lines)))))
    (should-not (calligrus--alive-p))
    (should-not calligrus-preview-mode)
    (should-not (memq 'calligrus--kill-emacs kill-emacs-hook))))

(ert-deftest calligrus-test-eof-when-stopped ()
  (calligrus-test--session
    (calligrus-test--start)
    (calligrus-stop)
    (should (calligrus-test--wait (lambda () (equal (car (last (calligrus-test--lines))) "EOF"))))))

(ert-deftest calligrus-test-process-death-and-restart ()
  (calligrus-test--session
    (let ((buf (calligrus-test--start)) (msgs nil))
      (cl-letf (((symbol-function 'message)
                 (lambda (fmt &rest args) (push (apply #'format fmt args) msgs))))
        (signal-process calligrus--process 'kill)
        (should (calligrus-test--wait (lambda () (not calligrus-preview-mode)))))
      (should-not (calligrus--alive-p))
      (should (cl-some (lambda (m) (string-match-p "preview window closed.*previews are off" m)) msgs))
      (should-not (memq 'calligrus--after-change after-change-functions))
      ;; toggle again: new process, new open
      (with-current-buffer buf
        (calligrus-toggle)
        (should calligrus-preview-mode))
      (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "open") 2))))
      (should (= (length (cl-remove "ARGS --editor" (calligrus-test--lines) :test-not #'equal)) 2)))))

(ert-deftest calligrus-test-missing-exe ()
  (calligrus-test--session
    (let ((calligrus-exe "no-such-program-calligrus-xyz") (msgs nil))
      (calligrus-test--buffer "a" "hi")
      (cl-letf (((symbol-function 'message)
                 (lambda (fmt &rest args) (push (apply #'format fmt args) msgs))))
        (calligrus-toggle))
      (should-not calligrus-preview-mode)
      (should (cl-some (lambda (m) (string-match-p "cannot run" m)) msgs)))))

(ert-deftest calligrus-test-open-file ()
  (calligrus-test--session
    (let ((file (make-temp-file "cg-sample" nil ".md")))
      (unwind-protect
          (progn
            (find-file file)
            (rename-buffer "cg-t-file")
            (calligrus-open-file)
            (should (calligrus-test--wait
                     (lambda () (cl-some (lambda (l) (string-match-p "\\`ARGS .*cg-sample" l))
                                         (calligrus-test--lines)))))
            (should-not (calligrus--alive-p)))
        (let ((kill-buffer-hook nil)) (kill-buffer "cg-t-file"))
        (delete-file file)))))

(ert-deftest calligrus-test-non-ascii-roundtrip ()
  (calligrus-test--session
    (let ((buf (calligrus-test--buffer "u" "日本語 é 😀 \"q\" \\ tab\there")))
      (calligrus-toggle)
      (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "open") 1))))
      (should (equal (alist-get 'text (car (calligrus-test--of "open")))
                     (with-current-buffer buf (buffer-string)))))))

;;; goto (Ctrl+click in the preview)

(defun calligrus-test--emit (s)
  "Make the stub write S to its stdout in one write; a literal backslash-n in S is a newline."
  (let ((coding-system-for-write 'utf-8-unix))
    (write-region (concat s "\n") nil calligrus-test--out t 'silent)))

(defun calligrus-test--goto (doc line)
  (calligrus-test--emit (format "{\"type\":\"goto\",\"doc\":\"%s\",\"line\":%s}\\n" doc line)))

(defun calligrus-test--shown ()
  "The buffer in the selected window (after a process filter ran, `current-buffer' is restored)."
  (window-buffer (selected-window)))

(defun calligrus-test--line ()
  "Line of point in the selected window."
  (with-current-buffer (calligrus-test--shown)
    (line-number-at-pos (window-point) t)))

(ert-deftest calligrus-test-notes-buffer ()
  (let ((calligrus-notes-buffer "*cg-t-notes*"))
    (unwind-protect
        (progn
          ;; no notes and no buffer yet: nothing appears
          (calligrus--handle-line "{\"type\":\"notes\",\"doc\":\"1\",\"slide\":1,\"total\":3,\"notes\":\"\"}")
          (should-not (get-buffer calligrus-notes-buffer))
          ;; a slide with notes: the buffer appears, with the slide number
          (calligrus--handle-line "{\"type\":\"notes\",\"doc\":\"1\",\"slide\":2,\"total\":3,\"notes\":\"say hi\\nthen go\"}")
          (with-current-buffer calligrus-notes-buffer
            (should (equal (buffer-string) "Slide 2 / 3\n\nsay hi\nthen go\n"))
            (should buffer-read-only))
          ;; once there, it follows slides without notes too
          (calligrus--handle-line "{\"type\":\"notes\",\"doc\":\"1\",\"slide\":3,\"total\":3,\"notes\":\"\"}")
          (with-current-buffer calligrus-notes-buffer
            (should (equal (buffer-string) "Slide 3 / 3\n\n(no notes)\n")))
          ;; switched off: ignored; malformed: ignored
          (let ((calligrus-notes nil))
            (calligrus--handle-line "{\"type\":\"notes\",\"doc\":\"1\",\"slide\":1,\"total\":3,\"notes\":\"x\"}"))
          (calligrus--handle-line "{\"type\":\"notes\",\"slide\":\"one\"}")
          (with-current-buffer calligrus-notes-buffer
            (should (string-prefix-p "Slide 3 / 3" (buffer-string)))))
      (when (get-buffer calligrus-notes-buffer) (kill-buffer calligrus-notes-buffer)))))

(ert-deftest calligrus-test-follow-slides ()
  (calligrus-test--session
    (let* ((a (calligrus-test--start)) (doc (calligrus--doc a)) (calligrus-notes nil)
           (slide (lambda (line end)
                    (calligrus--handle-line
                     (format "{\"type\":\"notes\",\"doc\":\"%s\",\"slide\":2,\"total\":2,\"notes\":\"\",\"line\":%d,\"end\":%d}"
                             doc line end)))))
      ;; a new slide: point goes to its first line, and that counts as sent
      (goto-char (point-min))
      (funcall slide 3 5)
      (should (= (calligrus-test--line) 3))
      (should (eql (buffer-local-value 'calligrus--last-line a) 3))
      ;; point already on the slide: it stays
      (goto-char (point-max))
      (funcall slide 3 5)
      (should (= (calligrus-test--line) 5))
      ;; switched off: point stays
      (let ((calligrus-follow-slides nil))
        (funcall slide 1 2))
      (should (= (calligrus-test--line) 5))
      ;; a message without lines (an older calligrus): point stays
      (calligrus--handle-line (format "{\"type\":\"notes\",\"doc\":\"%s\",\"slide\":1,\"total\":2,\"notes\":\"\"}" doc))
      (should (= (calligrus-test--line) 5)))))

(ert-deftest calligrus-test-goto-moves-point ()
  (calligrus-test--session
    (let* ((a (calligrus-test--start)) (doc (calligrus--doc a)))
      ;; indent line 3, then look at some other buffer
      (goto-char (point-min)) (forward-line 2) (insert "    ")
      (let ((plain (calligrus-test--buffer "plain" "x" #'fundamental-mode)))
        (calligrus-test--goto doc 3)
        (should (calligrus-test--wait (lambda () (eq (calligrus-test--shown) a))))
        (should (= (calligrus-test--line) 3))
        (should (with-current-buffer a (save-excursion (goto-char (window-point)) (= (current-column) 4))))
        ;; the move produces a normal cursor message (and nothing else)
        (should (calligrus-test--wait (lambda () (equal (alist-get 'line (calligrus-test--last)) 3))))
        (should (equal (alist-get 'type (calligrus-test--last)) "cursor"))
        (let ((n (calligrus-test--count "cursor")))
          (calligrus-test--settle 0.3)
          (should (= n (calligrus-test--count "cursor"))))
        (should (buffer-live-p plain))))))

(ert-deftest calligrus-test-goto-uses-existing-window ()
  (calligrus-test--session
    (let* ((a (calligrus-test--start)) (doc (calligrus--doc a)) (win-a (selected-window)))
      (let ((win-b (split-window)))
        (select-window win-b)
        (switch-to-buffer (generate-new-buffer "cg-t-other"))
        (calligrus-test--goto doc 5)
        (should (calligrus-test--wait (lambda () (eq (selected-window) win-a))))
        (should (= (calligrus-test--line) 5))
        (should (= (length (window-list)) 2))
        (should-not (eq (window-buffer win-b) a))))))

(ert-deftest calligrus-test-goto-clamps ()
  (calligrus-test--session
    (let* ((a (calligrus-test--start)) (doc (calligrus--doc a)))
      (goto-char (point-min))
      (calligrus-test--goto doc 999)
      (should (calligrus-test--wait (lambda () (= (calligrus-test--line) 5))))
      (calligrus-test--goto doc 0)
      (should (calligrus-test--wait (lambda () (= (calligrus-test--line) 1))))
      ;; a buffer with a final newline: stop on its last real line
      (goto-char (point-min)) (goto-char (point-max)) (insert "\n")
      (goto-char (point-min))
      (calligrus-test--goto doc 999)
      (should (calligrus-test--wait (lambda () (= (calligrus-test--line) 5)))))))

(ert-deftest calligrus-test-goto-split-and-junk ()
  (calligrus-test--session
    (let* ((a (calligrus-test--start)) (doc (calligrus--doc a)))
      (goto-char (point-max))
      ;; one message in two writes: nothing happens until the second arrives
      (calligrus-test--emit (format "{\"type\":\"goto\",\"doc\":\"%s\",\"li" doc))
      (calligrus-test--settle 0.2)
      (should (= (calligrus-test--line) 5))
      (calligrus-test--emit "ne\":2}\\n")
      (should (calligrus-test--wait (lambda () (= (calligrus-test--line) 2))))
      ;; junk is ignored: malformed, wrong types, unknown type, wrong or unknown doc
      (goto-char (point-max))
      (calligrus-test--emit "this is not json\\n")
      (calligrus-test--emit (format "{\"type\":\"goto\",\"doc\":\"%s\",\"line\":\\n" doc))
      (calligrus-test--emit (format "{\"type\":\"goto\",\"doc\":\"%s\",\"line\":\"2\"}\\n" doc))
      (calligrus-test--emit (format "{\"type\":\"mystery\",\"doc\":\"%s\",\"line\":2}\\n" doc))
      (calligrus-test--emit "[1,2]\\n")
      (calligrus-test--emit "null\\n")
      (calligrus-test--goto "nope" 2)
      (calligrus-test--goto (1+ (string-to-number doc)) 2)
      (calligrus-test--settle 0.5)
      (should (eq (calligrus-test--shown) a))
      (should (= (calligrus-test--line) 5))
      ;; ... and it still works afterwards, also two messages in one chunk
      (calligrus-test--emit (concat (format "{\"type\":\"goto\",\"doc\":\"%s\",\"line\":1}\\n" doc)
                                 (format "{\"type\":\"goto\",\"doc\":\"%s\",\"line\":4}\\n" doc)))
      (should (calligrus-test--wait (lambda () (= (calligrus-test--line) 4)))))))

(ert-deftest calligrus-test-goto-off ()
  (calligrus-test--session
    (let* ((a (calligrus-test--start)) (doc (calligrus--doc a)))
      (goto-char (point-max))
      (let ((calligrus-goto nil))
        (calligrus-test--goto doc 1)
        (calligrus-test--settle 0.5)
        (should (= (calligrus-test--line) 5)))
      (calligrus-test--goto doc 1)
      (should (calligrus-test--wait (lambda () (= (calligrus-test--line) 1)))))))

(ert-deftest calligrus-test-goto-ignores-unpreviewed-doc ()
  (calligrus-test--session
    (let* ((a (calligrus-test--start)) (doc (calligrus--doc a)))
      (calligrus-toggle)                   ; off (muted): the doc is closed
      (goto-char (point-max))
      (calligrus-test--goto doc 1)
      (calligrus-test--settle 0.5)
      (should (= (calligrus-test--line) 5)))))

(ert-deftest calligrus-test-goto-reveals-outline ()
  (calligrus-test--session
    (let ((buf (calligrus-test--buffer "o" "* One\nbody one\n* Two\nbody two")))
      (with-current-buffer buf (outline-mode))
      (calligrus-toggle t)
      (should (calligrus-test--wait (lambda () (>= (calligrus-test--count "open") 1))))
      (outline-hide-body)
      (goto-char (point-min))
      (should (invisible-p (save-excursion (goto-char (point-min)) (forward-line 1) (point))))
      (calligrus-test--goto (calligrus--doc buf) 2)
      (should (calligrus-test--wait (lambda () (= (calligrus-test--line) 2))))
      (should-not (invisible-p (point))))))

;;; calligrus-test.el ends here
