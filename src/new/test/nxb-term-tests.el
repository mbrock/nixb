;;; nxb-term-tests.el --- YAML-driven terminal checks -*- lexical-binding: t; -*-

(require 'cl-lib)
(require 'subr-x)
(require 'ert)
(require 'yaml)

(defvar nxb-term--project-root
  (locate-dominating-file (or load-file-name default-directory) "flake.nix")
  "Root directory of the nixb project.")

(defvar nxb-term--configured-build-dirs nil)
(defvar nxb-term--built-targets nil)

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

(defun nxb-term--tests-from-data (data)
  "Extract the list of tests from DATA."
  (cond
   ((and (listp data) (alist-get 'tests data)) (alist-get 'tests data))
   ((listp data) data)
   (t (error "Unsupported YAML test structure: %S" data))))

(defun nxb-term--ensure-build (build)
  "Ensure BUILD (alist) is configured and its target built."
  (when build
    (let* ((dir (or (alist-get 'dir build) "build"))
           (setup-args (or (alist-get 'setup-args build) '()))
           (target (alist-get 'target build))
           (ninja-args (or (alist-get 'ninja-args build) '()))
           (abs-dir (expand-file-name dir nxb-term--project-root)))
      (unless (gethash abs-dir nxb-term--configured-build-dirs)
        (unless (file-directory-p (expand-file-name "meson-info" abs-dir))
          (nxb-term--run-command
           (append (list "meson" "setup" dir "--wrap-mode=nodownload")
                   setup-args)))
        (puthash abs-dir t nxb-term--configured-build-dirs))
      (when target
        (let ((key (cons abs-dir target)))
          (unless (gethash key nxb-term--built-targets)
            (nxb-term--run-command
             (append (list "ninja" "-C" dir target) ninja-args))
            (puthash key t nxb-term--built-targets))))
      dir)))

(defun nxb-term--sanitize-name (file name)
  "Generate a stable symbol name from FILE and NAME."
  (let* ((base (file-name-base file))
         (slug (replace-regexp-in-string "[^[:alnum:]]+" "-" (or name "anon"))))
    (intern (format "nxb-yaml-%s-%s" base slug))))

(defun nxb-term--resolve-command (spec build-dir target)
  "Resolve command from SPEC, BUILD-DIR, and TARGET."
  (let ((cmd (alist-get 'command spec)))
    (cond
     ((stringp cmd) cmd)
     ((and cmd (listp cmd)) cmd)
     (target (format "%s/%s" (or build-dir "build") target))
     (t (error "Test %S requires a command or build target" (alist-get 'name spec))))))

(defun nxb-term--define-test (spec file)
  "Define a single test from SPEC originating from FILE."
  (let* ((name (alist-get 'name spec))
         (test-sym (nxb-term--sanitize-name file name))
         (size (alist-get 'size spec))
         (width (or (alist-get 'width size) 20))
         (height (or (alist-get 'height size) 6))
         (expect (alist-get 'expect spec))
         (disp (or (alist-get 'display expect)
                   (error "Test %s missing expected display" name)))
         (scroll (or (alist-get 'scrollback expect) '()))
         (cursor (nxb-term--maybe-cons-cursor (alist-get 'cursor expect)))
         (build (alist-get 'build spec))
         (build-dir (and build (or (alist-get 'dir build) "build")))
         (target (and build (alist-get 'target build)))
         (command (nxb-term--resolve-command spec build-dir target)))
    (when (fboundp test-sym) (fmakunbound test-sym))
    (eval
     `(ert-deftest ,test-sym ()
        (eat--tests-with-term '( :width ,width :height ,height)
          (nxb-term--ensure-build ',build)
          (let ((output-str (nxb-term--run-command ',command)))
            (output output-str)
            (should-term :scrollback ',scroll
                         :display ',disp
                         ,@(when cursor `(:cursor ',cursor)))))))))

(defun nxb-term--files-from-env ()
  "Return YAML test file list from $NXB_TTYTEST_FILES."
  (when-let ((env (getenv "NXB_TTYTEST_FILES")))
    (cl-remove-if #'string-empty-p (split-string env "\n"))))

(defun nxb-run-yaml-tests (files)
  "Load YAML tests from FILES (list or string) and run them via ERT.
When FILES is nil, read file list from the NXB_TTYTEST_FILES environment
variable, defaulting to \"src/new/test/tests.yaml\"."
  (let* ((file-list (cond
                     ((null files)
                      (or (nxb-term--files-from-env)
                          '("src/new/test/tests.yaml")))
                     ((stringp files) (list files))
                     ((listp files) files)
                     (t (error "Unsupported FILES argument: %S" files)))))
    (setq nxb-term--configured-build-dirs (make-hash-table :test 'equal))
    (setq nxb-term--built-targets (make-hash-table :test 'equal))
    (dolist (file file-list)
      (let* ((data (nxb-term--load-yaml file))
             (tests (nxb-term--tests-from-data data)))
        (dolist (spec tests)
          (nxb-term--define-test spec file))))
    (ert-run-tests-batch-and-exit "^nxb-yaml-")))

(provide 'nxb-term-tests)
