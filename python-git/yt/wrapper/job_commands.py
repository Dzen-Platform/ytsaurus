
def run_job_shell(job_id, timeout=None, client=None):
    """ Run interactive shell in the job sandbox

    :param job_id: (string) job id
    """
    from job_shell import JobShell

    JobShell(job_id, interactive=True, timeout=timeout, client=client).run()
