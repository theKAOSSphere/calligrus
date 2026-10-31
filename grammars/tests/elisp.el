;;; demo.el --- a small example  -*- lexical-binding: t -*-

(defvar my-counter 0
  "How many times `my-greet' ran.")

(defun my-greet (name &optional loud)
  "Greet NAME, shouting when LOUD is non-nil."
  (interactive "sName: ")
  (let ((msg (format "Hello, %s!" name)))
    (setq my-counter (1+ my-counter))
    (if loud
        (message "%s" (upcase msg))
      (message msg))))

(add-hook 'after-init-hook #'my-greet)
(when (and t (not nil)) (my-greet "world" :loud ?a 3.5))
