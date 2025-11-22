;;; nxb-term-tests.el --- YAML-driven terminal checks -*- lexical-binding: t; -*-

(require 'cl-lib)
(require 'ert)
(require 'yaml)

(defvar nxb-term--project-root
  (locate-dominating-file (getenv "PWD") "flake.nix")
  "Root directory of the nixb project.")

(load (expand-file-name "subprojects/eat/eat-tests.el" nxb-term--project-root))

(defun nxb-term--load-yaml (path)
  "Load YAML PATH and return it as Lisp data."
  (let ((yaml (expand-file-name path nxb-term--project-root)))
    (with-temp-buffer
      (insert-file-contents yaml)
      (yaml-parse-string (buffer-string)
                         :object-type 'alist
                         :sequence-type 'list
                         :null-object nil
                         :false-object nil))))

(defun nxb-term--run-command (command)
  "Run COMMAND (string or list) and capture stdout as string."
  (let* ((default-directory nxb-term--project-root)
         (program (cond
                   ((listp command) (car command))
                   ((stringp command) "sh")
                   (t (error "Invalid command spec: %S" command))))
         (args (cond
                ((listp command) (cdr command))
                ((stringp command) (list "-c" command))
                (t nil)))
         (buf (generate-new-buffer " *nxb-cmd*")))
    (unwind-protect
        (let ((exit (apply #'call-process program nil buf nil args)))
          (unless (and (integerp exit) (zerop exit))
            (error "Command %s failed with %s: %s"
                   command exit (with-current-buffer buf (buffer-string))))
          (with-current-buffer buf (buffer-string)))
      (kill-buffer buf))))

(defun nxb-term--maybe-cons-cursor (cursor)
  (when (and (listp cursor) (= (length cursor) 2))
    (cons (car cursor) (cadr cursor))))

(defun nxb-term--define-test (spec)
  (let* ((name (alist-get 'name spec))
         (test-sym (intern (format "nxb-yaml-%s" name)))
         (command (alist-get 'command spec))
         (size (alist-get 'size spec))
         (width (or (alist-get 'width size) 20))
         (height (or (alist-get 'height size) 6))
         (expect (alist-get 'expect spec))
         (disp (alist-get 'display expect))
         (scroll (alist-get 'scrollback expect))
         (cursor (nxb-term--maybe-cons-cursor (alist-get 'cursor expect))))
    (when (fboundp test-sym) (fmakunbound test-sym))
    (eval
     `(ert-deftest ,test-sym ()
        (eat--tests-with-term '( :width ,width :height ,height)
          (let ((output-str (nxb-term--run-command ',command)))
            (output output-str)
            (should-term :scrollback ',scroll
                         :display ',disp
                         ,@(when cursor `(:cursor ',cursor)))))))))

(defun nxb-run-yaml-tests (relative-path)
  "Load YAML tests from RELATIVE-PATH and run them via ERT."
  (let* ((data (nxb-term--load-yaml relative-path)))
    (mapc #'nxb-term--define-test data)
    (ert-run-tests-batch-and-exit "^nxb-yaml-")))

(provide 'nxb-term-tests)
