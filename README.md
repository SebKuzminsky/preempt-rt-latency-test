kernel command line should include:
    rcu_nocb_poll
    rcu_nocbs=N
    isolcpus=nohz,domain,managed_irq,N
    nohz_full=N
    irqaffinity=0-M

`N` is the highest CPU in the system (e.g. "3" on a 4-CPU system)

`M` is one less than the highest CPU in the system (e.g. "2" on a
4-CPU system)
